// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.Page {
    id: page
    objectName: "playerPage"
    required property var player
    required property ConnectionController connection
    // Watching together (SyncPlayController), null when the application gives none: in a group the speed, shuffle and repeat are the group's
    property var syncPlay: null
    readonly property bool grouped: syncPlay?.status === SyncPlayController.Joined
    // The group takes the player's actions: joined, following and online
    readonly property bool groupFollowing: grouped && syncPlay.following && !syncPlay.offline
    // The video item of the shell sits here while this page is in front
    readonly property Item videoSlot: slot
    title: player?.title || i18n("Now Playing")
    padding: 0
    // The video fills the page and the bars float above it: the page row's toolbar would be a strip that never hides
    globalToolBarStyle: Kirigami.ApplicationHeaderStyle.None
    readonly property bool isShowing: page.Window.window?.pageStack.currentItem === page
    onIsShowingChanged: {
        if (isShowing)
            return;
        volumePopup.close();
    }
    property bool showChrome: true
    // Below the window's wide limit the controls row shows icons only, each with its text as tooltip and accessible name
    readonly property bool narrow: width < (page.Window.window?.wideLimit ?? 0)
    // The button at the end of the timeline shows the remaining time instead of the total, for as long as this page lives
    property bool showRemaining: false
    readonly property bool fullScreen: page.Window.window?.visibility === Window.FullScreen
    // An unknown length has neither a total nor a remaining time
    readonly property string endTimeLabel: {
        if (page.player.durationTicks <= 0)
            return "";
        if (!page.showRemaining)
            return page.player.timeText(page.player.durationTicks);
        return i18nc("@action:button remaining playback time, with a minus sign", "−%1", page.player.timeText(Math.max(0, page.player.durationTicks - page.player.positionTicks)));
    }
    // The widest the elapsed time gets for this item: the total stands for every time (none is longer), and with tabular digits every digit is as wide as 0
    readonly property string widestElapsedLabel: page.player.timeText(Math.max(page.player.durationTicks, page.player.positionTicks)).replace(/[0-9]/g, "0")
    // The widest the end time button gets, whatever it shows: the remaining time adds its minus sign to the total
    readonly property string widestEndTimeLabel: i18nc("@action:button remaining playback time, with a minus sign", "−%1", page.player.timeText(page.player.durationTicks)).replace(/[0-9]/g, "0")
    // Set by tests: the clock time the end of the item is counted from, in place of the real one
    property var fixedNow: null
    // Counts the 20 s ticks of the clock, so the end time follows it while nothing else changes, as in a pause
    property int clockTick: 0
    function endsAtFor(now, tick) {
        const left = page.player.durationTicks - page.player.positionTicks;
        if (page.player.durationTicks <= 0 || left <= 0)
            return "";
        // A tick is 100 ns; half a minute is added so that cutting the seconds off rounds to the nearest minute
        const end = new Date(now.getTime() + left / 10000 / Math.max(0.01, page.player.rate) + 30000);
        end.setSeconds(0, 0);
        return i18nc("@info:status", "Ends at %1", end.toLocaleTimeString(Qt.locale(), Locale.ShortFormat));
    }
    // The position, the duration and the rate are the inputs: a seek or a new rate moves the end at once
    readonly property string endsAtText: page.endsAtFor(page.fixedNow ?? new Date(), page.clockTick)
    // The size of the video item in device pixels
    readonly property size videoPixelSize: {
        const ratio = page.Window.window?.devicePixelRatio ?? 1;
        return Qt.size(Math.round(slot.width * ratio), Math.round(slot.height * ratio));
    }
    // Whether the Playback Info panel is open
    property bool playbackInfoShown: false
    function closePlaybackInfo() {
        page.playbackInfoShown = false;
        page.focusPrimary();
    }
    // The item's chapters, read from the player once per change rather than by each binding that needs them
    readonly property var chapters: page.player.chapters
    // An open menu or the queue panel keeps the bars up
    readonly property bool menuOpen: queuePanel.opened || audioMenu.opened || subtitlesMenu.opened || moreMenu.opened || contextMenu.opened
    // The lock keeps the bars up until it is opened again
    property bool osdLocked: false
    // What else keeps the bars up whatever the timer says: the lock, a pause, the pointer on a bar, a drag of the timeline
    readonly property bool barsHeld: osdLocked || page.player.paused || page.player.casting || pointerOnBars || timeline.pressed
    // The pointer in the window over a bar, its controls or its shade, read from the one hover of the page: it follows every move and ends when the pointer leaves the window
    readonly property bool pointerOnBars: pointer.hovered && osd.visible && (pointer.point.position.y < topBar.height || pointer.point.position.y > page.height - bottomBar.height)
    // Once nothing holds them, the bars get their full time again before they go
    onBarsHeldChanged: if (!barsHeld && showChrome) chromeTimer.restart()
    // The volume slider keeps the bars up only while the pointer is on it or on the speaker, or drags it: one opened by the keyboard or a long press goes with the bars
    readonly property bool volumeHeld: volumePopup.opened && (muteButton.hovered || volumeHover.hovered || volumeSlider.pressed)

    // The page keeps the keyboard: the controls of the bars take it only through Tab, so the keys work whatever was clicked
    function focusPrimary() { page.forceActiveFocus(Qt.TabFocusReason); }
    function wakeChrome() { showChrome = true; chromeTimer.restart(); }
    // Leaving the page keeps the playback going: the shell parks the video in the mini player before the page goes
    // As Kirigami's Back does, the page in front only moves back one place; the shell takes the page left above once the stack settles, which deletes it (a pop of the page in front does not)
    function leavePlayer() {
        const shell = page.Window.window;
        shell.parkVideo();
        shell.pageStack.currentIndex = Math.max(0, shell.pageStack.currentIndex - 1);
    }
    // The video moves into the picture-in-picture window and the page goes
    function pictureInPicture() {
        const shell = page.Window.window;
        shell.enterPictureInPicture();
    }
    function closePlayer() { page.Window.window.stopPlayback(); }
    // When a menu or popup last began to close: the click that closes one is not a click on the video
    property double popupClosedAt: 0
    function popupGone() {
        popupClosedAt = Date.now();
        // The page takes the keys again, so Space plays or pauses instead of pressing the button that opened the menu
        page.forceActiveFocus(Qt.PopupFocusReason);
        wakeChrome();
    }
    // Once a popup is fully closed, after Qt put the focus where it wants: the page takes it again unless another popup is open
    function popupClosed() {
        if (!page.Window.window?.popupOpen?.())
            page.forceActiveFocus(Qt.PopupFocusReason);
    }
    // The volume slider pops up centred above the speaker button
    function showVolume() {
        volumeCloser.stop();
        // A long press on a touch screen has no hover to hold the bars: they stay their full time from here
        page.wakeChrome();
        volumePopup.open();
    }
    function stepVolume(steps) {
        page.Window.window.stepVolume(steps);
        page.wakeChrome();
    }
    // The player commands of the keyboard: the shell runs them, in every mode, and the page only wakes its bars
    function playerAction(name) {
        page.Window.window.playerAction(name);
        page.wakeChrome();
    }
    // A menu of the bottom bar opens above its button, aligned to the button's far edge
    function openMenu(menu, button) {
        const corner = button.mapToItem(page, page.mirrored ? 0 : button.width, 0);
        menu.popup(page.mirrored ? corner.x : corner.x - menu.implicitWidth, corner.y - menu.implicitHeight);
    }
    // The commands and labels the bars and the context menu share
    readonly property bool canTogglePause: page.player.active && !page.player.loading
    readonly property string pauseText: page.player.paused ? i18n("Resume") : i18n("Pause")
    readonly property string pauseIcon: page.player.paused ? "media-playback-start" : "media-playback-pause"
    readonly property string fullScreenText: page.fullScreen ? i18nc("@action:button", "Exit Full Screen Mode") : i18nc("@action:button", "Full Screen Mode")
    function togglePlayback() { page.player.togglePause(); page.wakeChrome(); }
    function skipPrevious() { page.player.previous(); page.wakeChrome(); }
    function skipNext() { page.player.next(); page.wakeChrome(); }
    function toggleFullScreen() { page.Window.window.togglePlayerFullscreen(); page.wakeChrome(); }
    // The context menu opens at the pointer or, from the keyboard, at the centre of the video; never over another popup
    function showContextMenu(x, y) {
        page.wakeChrome();
        if (page.menuOpen || volumePopup.opened)
            return false;
        contextMenu.popup(x, y);
        return true;
    }
    function focusIsInOsd() {
        for (let item = page.Window.window?.activeFocusItem; item; item = item.parent) {
            if (item === osd)
                return true;
        }
        return false;
    }

    // Esc, Alt+Left and the mouse back button close the queue panel and the Playback Info panel first, then cancel the Up Next card, then leave a full screen the player entered, and only then leave the player
    onBackRequested: event => {
        if (volumePopup.opened) {
            volumePopup.close();
            event.accepted = true;
        } else if (queuePanel.opened) {
            queuePanel.close();
            event.accepted = true;
        } else if (page.playbackInfoShown) {
            page.closePlaybackInfo();
            event.accepted = true;
        } else if (page.player.upNextShown) {
            page.player.cancelUpNext();
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
        if (event.isAutoRepeat && (event.key === Qt.Key_Space || event.key === Qt.Key_M || event.key === Qt.Key_F || event.key === Qt.Key_Menu || (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier)))) {
            event.accepted = true;
            return;
        }
        switch (event.key) {
        case Qt.Key_Space: page.playerAction("playpause"); break;
        case Qt.Key_M: page.playerAction("togglemute"); break;
        case Qt.Key_F: page.playerAction("togglefullscreen"); break;
        case Qt.Key_I: page.player.markClipStart(); break;
        case Qt.Key_O: page.player.markClipEnd(); break;
        case Qt.Key_Left: page.playerAction("rewind"); break;
        case Qt.Key_Right: page.playerAction("fastforward"); break;
        // The minute seeks have no command name: only the keyboard has them
        case Qt.Key_Up: page.player.seekRelative(60); break;
        case Qt.Key_Down: page.player.seekRelative(-60); break;
        // As mpv's own keys: Page Up goes on to the next chapter
        case Qt.Key_PageUp: page.player.stepChapter(1); break;
        case Qt.Key_PageDown: page.player.stepChapter(-1); break;
        // Play Now on the Up Next card, the remote's select included; without the card Return does nothing here
        case Qt.Key_Return:
        case Qt.Key_Enter:
            if (!page.player.upNextShown)
                return;
            page.player.playUpNext();
            break;
        case Qt.Key_Plus:
        case Qt.Key_Equal: page.playerAction("volumeup"); break;
        case Qt.Key_Minus: page.playerAction("volumedown"); break;
        // The Menu key and Shift+F10 open the context menu at the centre of the video
        case Qt.Key_F10:
            if (!(event.modifiers & Qt.ShiftModifier))
                return;
        // fall through
        case Qt.Key_Menu:
            page.showContextMenu(page.width / 2, page.height / 2);
            break;
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
            if (!page.player.active || page.menuOpen || page.volumeHeld || page.barsHeld)
                return;
            // A control of a hidden bar would keep the keys: the page takes them back. The control gives its focus up first, or the page, a focus scope, would hand it back to it unseen when the bars show again
            if (page.focusIsInOsd()) {
                page.Window.window.activeFocusItem.focus = false;
                page.focusPrimary();
            }
            page.showChrome = false;
            volumePopup.close();
        }
    }
    Timer {
        id: clockTimer
        objectName: "playerClockTimer"
        interval: 20000
        repeat: true
        running: page.isShowing
        onTriggered: page.clockTick++
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
    Rectangle { anchors.fill: parent; color: "black" }
    Item {
        id: slot
        objectName: "playerVideoSlot"
        anchors.fill: parent
    }
    // A cast has no picture here: the device plays it, and this says where
    ColumnLayout {
        objectName: "playerCastView"
        anchors.centerIn: parent
        width: Math.min(parent.width - Kirigami.Units.gridUnit * 2, Kirigami.Units.gridUnit * 24)
        spacing: Kirigami.Units.largeSpacing
        visible: page.player.casting
        Kirigami.Theme.colorSet: Kirigami.Theme.Complementary
        Kirigami.Theme.inherit: false
        Kirigami.Icon {
            Layout.alignment: Qt.AlignHCenter
            implicitWidth: Kirigami.Units.iconSizes.huge
            implicitHeight: Kirigami.Units.iconSizes.huge
            source: "video-television"
        }
        Controls.Label {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: i18nc("@info:status %1 is a device name", "Playing on %1", page.player.castTargetName)
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: Kirigami.Theme.textColor
        }
        Controls.Button {
            Layout.alignment: Qt.AlignHCenter
            text: i18nc("@action:button", "Stop Casting")
            icon.name: "media-playback-stop"
            onClicked: page.player.stopCasting()
        }
    }
    // Only a move of the pointer wakes the bars: the scene repeats the last hover position on every frame that changes something under it. The pointer hides with the bars
    HoverHandler {
        id: pointer
        objectName: "playerPointer"
        cursorShape: osd.wanted ? Qt.ArrowCursor : Qt.BlankCursor
        property point lastPosition: Qt.point(-1, -1)
        onPointChanged: {
            if (point.position.x === lastPosition.x && point.position.y === lastPosition.y)
                return;
            lastPosition = point.position;
            page.wakeChrome();
        }
    }
    // A click on the video pauses or resumes after a short wait, so the usual double click, which toggles full screen, never stops the video; a slower double click puts the pause back as the click before it changed it; a tap on a touch screen only shows the bars
    TapHandler {
        id: videoTap
        // Whether the last click of this run paused or resumed: the double click undoes it
        property bool toggledPause: false
        onTapped: (eventPoint, button) => {
            page.wakeChrome();
            // Only the first click of a run pauses: the second is a double click, a third neither
            if (videoTap.tapCount > 1)
                return;
            videoTap.toggledPause = false;
            // A click that closed a menu or popup is spent on it
            if (eventPoint.device.type === PointerDevice.TouchScreen || page.menuOpen || volumePopup.opened || Date.now() - page.popupClosedAt < Qt.styleHints.mouseDoubleClickInterval)
                return;
            // Nothing to pause while an item opens or after it failed
            if (page.player.active && !page.player.loading)
                clickPauseTimer.restart();
        }
        onDoubleTapped: (eventPoint, button) => {
            if (eventPoint.device.type === PointerDevice.TouchScreen)
                return;
            // The second click came in time: the first one pauses nothing
            if (clickPauseTimer.running) {
                clickPauseTimer.stop();
            } else if (videoTap.toggledPause) {
                page.player.togglePause();
                videoTap.toggledPause = false;
            }
            page.Window.window.togglePlayerFullscreen();
            page.wakeChrome();
        }
    }
    // A right click on the video opens the context menu at the pointer: it neither pauses nor toggles full screen, and the click that closes a menu or popup is spent on it
    TapHandler {
        objectName: "playerContextTap"
        acceptedButtons: Qt.RightButton
        onTapped: (eventPoint, button) => {
            if (eventPoint.device.type === PointerDevice.TouchScreen || Date.now() - page.popupClosedAt < Qt.styleHints.mouseDoubleClickInterval) {
                page.wakeChrome();
                return;
            }
            page.showContextMenu(eventPoint.position.x, eventPoint.position.y);
        }
    }
    // The wait of a click before it pauses: shorter than the double-click interval of the system (400 ms in Plasma), long enough for a usual double click
    Timer {
        id: clickPauseTimer
        objectName: "playerClickPauseTimer"
        interval: 250
        onTriggered: {
            if (!page.player.active || page.player.loading)
                return;
            const wasPaused = page.player.paused;
            page.player.togglePause();
            videoTap.toggledPause = page.player.paused !== wasPaused;
        }
    }

    // The group waits for a member: what this player shows meanwhile, also when the bars hide
    ColumnLayout {
        objectName: "playerGroupWaiting"
        anchors.centerIn: parent
        visible: page.groupFollowing && page.syncPlay.waiting && !page.player.loading && !page.player.errorText
        spacing: Kirigami.Units.largeSpacing
        FadeInOnShow {}
        Controls.BusyIndicator {
            Layout.alignment: Qt.AlignHCenter
            running: visible
        }
        Controls.Label {
            Layout.alignment: Qt.AlignHCenter
            color: "white"
            text: i18n("Waiting for the others…")
        }
    }

    // A member of the group came or went: named for a while under the top bar
    Controls.Label {
        id: groupNotice
        objectName: "playerGroupNotice"
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: topBar.height
        visible: noticeTimer.running
        color: "white"
        textFormat: Text.PlainText
        Accessible.role: Accessible.AlertMessage
        Accessible.name: text
        background: Rectangle {
            color: Qt.alpha("black", 0.6)
            radius: Kirigami.Units.cornerRadius
        }
        padding: Kirigami.Units.largeSpacing
        Timer {
            id: noticeTimer
            interval: 4000
        }
    }
    Connections {
        target: page.grouped ? page.syncPlay : null
        function onMemberJoined(name) {
            groupNotice.text = name.length > 0 ? i18nc("@info %1 is a user name", "%1 joined the SyncPlay group", name) : i18n("Someone joined the SyncPlay group");
            noticeTimer.restart();
        }
        function onMemberLeft(name) {
            groupNotice.text = name.length > 0 ? i18nc("@info %1 is a user name", "%1 left the SyncPlay group", name) : i18n("Someone left the SyncPlay group");
            noticeTimer.restart();
        }
    }

    // The loading and error overlay stays visible when the bars hide
    ColumnLayout {
        anchors.centerIn: parent
        visible: page.player.loading || !!page.player.errorText
        spacing: Kirigami.Units.largeSpacing
        FadeInOnShow {}
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
        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            spacing: Kirigami.Units.smallSpacing
            // A lowered stream that did not open: it can be opened again where it stopped, at the bit rate before the step
            Controls.Button {
                objectName: "playerRetryButton"
                visible: !!page.player.errorText && !!page.player.canRetry
                // The primary action of the overlay when there is one, unless the item can go on here
                highlighted: !page.player.canContinueHere
                text: i18nc("@action:button", "Try Again")
                icon.name: "view-refresh"
                onClicked: page.player.retryPlayback()
            }
            // A device failed: the item goes on here from where it stopped
            Controls.Button {
                objectName: "playerContinueHereButton"
                visible: page.player.canContinueHere
                highlighted: true
                text: i18nc("@action:button", "Continue Here")
                icon.name: "computer"
                onClicked: page.player.castTo("local")
            }
            Controls.Button {
                objectName: "playerCloseAfterErrorButton"
                visible: !!page.player.errorText
                text: i18n("Close Player")
                onClicked: page.closePlayer()
            }
        }
    }
    Controls.Button {
        id: skipButton
        objectName: "skipSegmentButton"
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Kirigami.Units.largeSpacing
        anchors.bottomMargin: bottomBar.height + Kirigami.Units.largeSpacing
        // The Up Next card takes its place over the credits; Cancel brings it back
        readonly property bool shown: page.player.skipLabel.length > 0 && !page.player.upNextShown
        // The last label stays while the button fades out, so it keeps its width
        property string shownLabel
        Binding on shownLabel {
            when: page.player.skipLabel.length > 0
            value: page.player.skipLabel
        }
        enabled: shown
        opacity: shown ? 1 : 0
        visible: shown || opacity > 0
        Behavior on opacity {
            NumberAnimation { duration: Kirigami.Units.longDuration; easing.type: Easing.OutCubic }
        }
        text: shownLabel
        icon.name: "media-skip-forward"
        onClicked: { page.player.skipSegment(); page.wakeChrome(); }
    }
    // A clip or GIF in the making, outside the bars like Up Next: Cancel stops it and removes the file
    Rectangle {
        id: clipCard
        objectName: "playerClipCard"
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Kirigami.Units.largeSpacing
        anchors.bottomMargin: bottomBar.height + Kirigami.Units.largeSpacing
        enabled: page.player.clipSaving
        visible: page.player.clipSaving
        width: Math.min(clipRow.implicitWidth + Kirigami.Units.largeSpacing * 2, page.width - Kirigami.Units.largeSpacing * 2)
        height: clipRow.implicitHeight + Kirigami.Units.largeSpacing * 2
        radius: Kirigami.Units.cornerRadius
        Kirigami.Theme.colorSet: Kirigami.Theme.Complementary
        Kirigami.Theme.inherit: false
        color: Qt.alpha(Kirigami.Theme.backgroundColor, 0.9)
        border.width: 1
        border.color: Qt.alpha(Kirigami.Theme.textColor, 0.2)
        Accessible.role: Accessible.Grouping
        Accessible.name: i18nc("@info accessible name of the card", "Saving the clip")
        BarClickGuard {}
        RowLayout {
            id: clipRow
            anchors.fill: parent
            anchors.margins: Kirigami.Units.largeSpacing
            spacing: Kirigami.Units.largeSpacing
            ColumnLayout {
                spacing: Kirigami.Units.smallSpacing
                Controls.Label {
                    objectName: "playerClipProgressLabel"
                    text: page.player.clipSavingGif
                          ? i18n("Saving the GIF: %1%", Math.round(page.player.clipProgress * 100))
                          : i18n("Saving the clip: %1%", Math.round(page.player.clipProgress * 100))
                }
                Controls.ProgressBar {
                    objectName: "playerClipProgress"
                    Layout.fillWidth: true
                    from: 0
                    to: 1
                    value: page.player.clipProgress
                }
            }
            Controls.Button {
                objectName: "playerClipCancel"
                focusPolicy: Qt.TabFocus
                text: i18n("Cancel")
                onClicked: page.player.cancelClip()
            }
        }
    }
    // Up Next, outside the bars like Skip: its buttons take the keyboard only through Tab, so Return (Play Now) and Esc (Cancel) stay with the page
    Rectangle {
        id: upNextCard
        objectName: "playerUpNextCard"
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Kirigami.Units.largeSpacing
        anchors.bottomMargin: bottomBar.height + Kirigami.Units.largeSpacing
        enabled: page.player.upNextShown
        opacity: page.player.upNextShown ? 1 : 0
        visible: page.player.upNextShown || opacity > 0
        Behavior on opacity {
            NumberAnimation { duration: Kirigami.Units.longDuration; easing.type: Easing.OutCubic }
        }
        // The player empties the next item as the card goes: it fades out with what it showed
        property var shownItem: ({})
        property int shownSeconds: 0
        Binding on shownItem {
            when: page.player.upNextShown
            value: page.player.upNext
        }
        Binding on shownSeconds {
            when: page.player.upNextShown
            value: page.player.upNextSeconds
        }
        width: Math.min(upNextRow.implicitWidth + Kirigami.Units.largeSpacing * 2, page.width - Kirigami.Units.largeSpacing * 2)
        height: upNextRow.implicitHeight + Kirigami.Units.largeSpacing * 2
        radius: Kirigami.Units.cornerRadius
        Kirigami.Theme.colorSet: Kirigami.Theme.Complementary
        Kirigami.Theme.inherit: false
        color: Qt.alpha(Kirigami.Theme.backgroundColor, 0.9)
        // An outline keeps it apart from a dark picture
        border.width: 1
        border.color: Qt.alpha(Kirigami.Theme.textColor, 0.2)
        Accessible.role: Accessible.Grouping
        Accessible.name: i18nc("@info accessible name of the card", "Up Next")
        // A click beside the buttons is not one on the video
        BarClickGuard {}
        RowLayout {
            id: upNextRow
            anchors.fill: parent
            anchors.margins: Kirigami.Units.largeSpacing
            spacing: Kirigami.Units.largeSpacing
            Image {
                objectName: "playerUpNextImage"
                Layout.preferredHeight: Kirigami.Units.gridUnit * 5
                Layout.preferredWidth: Math.round(Kirigami.Units.gridUnit * 5 * 16 / 9)
                Layout.alignment: Qt.AlignTop
                visible: source.toString().length > 0
                source: upNextCard.shownItem.imageUrl ?? ""
                sourceSize.height: Kirigami.Units.gridUnit * 5
                fillMode: Image.PreserveAspectFit
                asynchronous: true
            }
            ColumnLayout {
                Layout.maximumWidth: Kirigami.Units.gridUnit * 16
                spacing: Kirigami.Units.smallSpacing
                // The card's accessible name says it already
                Kirigami.Heading {
                    level: 4
                    text: i18nc("@title the card of the next item", "Up Next")
                    Accessible.ignored: true
                }
                Controls.Label {
                    objectName: "playerUpNextTitle"
                    Layout.fillWidth: true
                    textFormat: Text.PlainText
                    maximumLineCount: 1
                    elide: Text.ElideRight
                    font.bold: true
                    text: upNextCard.shownItem.title ?? ""
                }
                Controls.Label {
                    objectName: "playerUpNextSubtitle"
                    Layout.fillWidth: true
                    visible: text.length > 0
                    textFormat: Text.PlainText
                    maximumLineCount: 1
                    elide: Text.ElideRight
                    text: upNextCard.shownItem.subtitle ?? ""
                }
                Controls.Label {
                    objectName: "playerUpNextCountdown"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: i18np("Starts in %1 second", "Starts in %1 seconds", upNextCard.shownSeconds)
                }
                RowLayout {
                    spacing: Kirigami.Units.smallSpacing
                    // Reached with Tab, a button takes Return itself, before the page reads it as Play Now
                    Controls.Button {
                        objectName: "playerUpNextPlay"
                        focusPolicy: Qt.TabFocus
                        Keys.onReturnPressed: clicked()
                        Keys.onEnterPressed: clicked()
                        icon.name: "media-playback-start"
                        text: i18nc("@action:button starts the next item at once", "Play Now")
                        Accessible.name: i18nc("@action:button accessible name", "Play the Next Item Now")
                        onClicked: page.player.playUpNext()
                    }
                    Controls.Button {
                        objectName: "playerUpNextCancel"
                        focusPolicy: Qt.TabFocus
                        Keys.onReturnPressed: clicked()
                        Keys.onEnterPressed: clicked()
                        text: i18nc("@action:button the next item does not start", "Cancel")
                        Accessible.name: i18nc("@action:button accessible name", "Cancel Up Next")
                        onClicked: page.player.cancelUpNext()
                    }
                }
            }
        }
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
                objectName: "playerPreviewChapter"
                Layout.alignment: Qt.AlignHCenter
                Layout.maximumWidth: parent.width
                // Kept for the whole item, so the picture does not resize while a drag crosses into a chapter
                visible: page.chapters.length > 0
                color: "white"
                textFormat: Text.PlainText
                maximumLineCount: 1
                elide: Text.ElideRight
                text: {
                    const chapters = page.chapters;
                    const index = page.player.chapterAt(Math.floor(timeline.dragSeconds * 10000000));
                    return index >= 0 && index < chapters.length ? chapters[index].name : "";
                }
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
        readonly property bool wanted: page.showChrome || !page.player.active || page.menuOpen || page.volumeHeld || page.barsHeld
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
            // The bar slides out of the window as it fades
            transform: Translate {
                y: osd.wanted ? 0 : -Kirigami.Units.gridUnit
                Behavior on y {
                    NumberAnimation { duration: Kirigami.Units.longDuration; easing.type: Easing.OutCubic }
                }
            }
            BarClickGuard { onClicked: page.wakeChrome() }
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
                    onClicked: page.leavePlayer()
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
                // In a SyncPlay group: says so, and opens the group's dialog
                OsdButton {
                    objectName: "playerSyncPlayButton"
                    Layout.alignment: Qt.AlignTop
                    visible: page.grouped
                    display: page.narrow ? Controls.AbstractButton.IconOnly : Controls.AbstractButton.TextBesideIcon
                    icon.name: "system-users"
                    text: i18nc("@action:button", "Show SyncPlay Group")
                    onClicked: page.Window.window?.openSyncPlay()
                }
                // Where the video plays: a device for a video that plays here, the way back for a cast
                OsdButton {
                    id: castButton
                    objectName: "playerCastButton"
                    Layout.alignment: Qt.AlignTop
                    readonly property bool blockedByGroup: page.groupFollowing
                    display: page.narrow ? Controls.AbstractButton.IconOnly : Controls.AbstractButton.TextBesideIcon
                    visible: !page.player.localPlayback
                    enabled: !blockedByGroup
                    icon.name: "kinefin-cast"
                    text: i18nc("@action:button", "Cast…")
                    Accessible.description: blockedByGroup ? Controls.ToolTip.text : ""
                    Controls.ToolTip.text: blockedByGroup ? i18nc("@info:tooltip", "Not available while following a SyncPlay group") : text
                    Controls.ToolTip.visible: (hovered || visualFocus) && (display === Controls.AbstractButton.IconOnly || blockedByGroup)
                    onClicked: page.Window.window.castCurrent()
                }
                // The video in a window of its own; the mini player has no button here, it is the first entry of More Actions and Back leads to it as well
                OsdButton {
                    objectName: "playerPictureInPictureButton"
                    Layout.alignment: Qt.AlignTop
                    display: page.narrow ? Controls.AbstractButton.IconOnly : Controls.AbstractButton.TextBesideIcon
                    icon.name: "window-duplicate"
                    text: i18nc("@action:button", "Picture-in-Picture")
                    enabled: !page.player.casting
                    onClicked: page.pictureInPicture()
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
            transform: Translate {
                y: osd.wanted ? 0 : Kirigami.Units.gridUnit
                Behavior on y {
                    NumberAnimation { duration: Kirigami.Units.longDuration; easing.type: Easing.OutCubic }
                }
            }
            BarClickGuard { onClicked: page.wakeChrome() }
            ColumnLayout {
                id: controls
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: Kirigami.Units.largeSpacing
                spacing: Kirigami.Units.smallSpacing
                RowLayout {
                    objectName: "playerTimelineRow"
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.smallSpacing
                    Controls.Label {
                        id: elapsedLabel
                        objectName: "playerElapsedLabel"
                        // As wide as its widest text and never otherwise, so the timeline does not move while the numbers change
                        Layout.minimumWidth: widestElapsed.implicitWidth
                        Layout.preferredWidth: widestElapsed.implicitWidth
                        Layout.maximumWidth: widestElapsed.implicitWidth
                        horizontalAlignment: Text.AlignRight
                        font.features: { "tnum": 1 }
                        textFormat: Text.PlainText
                        text: page.player.timeText(page.player.positionTicks)
                    }
                    // The elapsed time label with its widest text, never shown
                    Controls.Label {
                        id: widestElapsed
                        visible: false
                        font: elapsedLabel.font
                        text: page.widestElapsedLabel
                    }
                    Controls.Slider {
                        id: timeline
                        objectName: "playbackTimeline"
                        Layout.fillWidth: true
                        focusPolicy: Qt.TabFocus
                        enabled: page.player.canSeek && page.player.durationTicks > 0
                        from: 0
                        to: Math.max(1, page.player.durationTicks / 10000000)
                        // No step size: a drag would land on its multiples (and the style would draw a tick mark at each); with the focus here the arrow keys seek as the page's do
                        Keys.onLeftPressed: { page.player.seekRelative(-10); page.wakeChrome(); }
                        Keys.onRightPressed: { page.player.seekRelative(10); page.wakeChrome(); }
                        Keys.onUpPressed: { page.player.seekRelative(60); page.wakeChrome(); }
                        Keys.onDownPressed: { page.player.seekRelative(-60); page.wakeChrome(); }
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
                        // The start and the end of the marked range, even alone: the band needs both
                        Rectangle {
                            objectName: "playerClipStartMark"
                            readonly property real fraction: page.player.durationTicks > 0 && page.player.clipStartTicks >= 0 ? Math.min(1, page.player.clipStartTicks / page.player.durationTicks) : 0
                            visible: page.player.clipStartTicks >= 0
                            x: timeline.leftPadding + timeline.handle.width / 2 + (timeline.mirrored ? 1 - fraction : fraction) * (timeline.availableWidth - timeline.handle.width)
                            y: Math.round(timeline.topPadding + (timeline.availableHeight - height) / 2)
                            width: 2
                            height: timeline.availableHeight
                            color: Kirigami.Theme.highlightColor
                            Accessible.ignored: true
                        }
                        Rectangle {
                            objectName: "playerClipEndMark"
                            readonly property real fraction: page.player.durationTicks > 0 && page.player.clipEndTicks >= 0 ? Math.min(1, page.player.clipEndTicks / page.player.durationTicks) : 0
                            visible: page.player.clipEndTicks >= 0
                            x: timeline.leftPadding + timeline.handle.width / 2 + (timeline.mirrored ? 1 - fraction : fraction) * (timeline.availableWidth - timeline.handle.width)
                            y: Math.round(timeline.topPadding + (timeline.availableHeight - height) / 2)
                            width: 2
                            height: timeline.availableHeight
                            color: Kirigami.Theme.highlightColor
                            Accessible.ignored: true
                        }
                        // The marked clip range, a translucent band under the chapter marks
                        Rectangle {
                            objectName: "playerClipBand"
                            readonly property real duration: page.player.durationTicks
                            readonly property real startFraction: duration > 0 && page.player.clipStartTicks >= 0 ? Math.min(1, page.player.clipStartTicks / duration) : 0
                            readonly property real endFraction: duration > 0 && page.player.clipEndTicks > page.player.clipStartTicks ? Math.min(1, page.player.clipEndTicks / duration) : startFraction
                            visible: page.player.clipStartTicks >= 0 && page.player.clipEndTicks > page.player.clipStartTicks
                            x: timeline.leftPadding + timeline.handle.width / 2 + (timeline.mirrored ? 1 - endFraction : startFraction) * (timeline.availableWidth - timeline.handle.width)
                            width: Math.abs(endFraction - startFraction) * (timeline.availableWidth - timeline.handle.width)
                            y: Math.round(timeline.topPadding + (timeline.availableHeight - height) / 2)
                            height: timeline.availableHeight
                            color: Qt.alpha(Kirigami.Theme.highlightColor, 0.35)
                            Accessible.ignored: true
                        }
                        // A thin cut in the groove where each chapter but the first starts, at the handle's middle for that time; the one under the handle hides with it, and the bar's own tick marks stay off
                        Item {
                            id: chapterMarks
                            // Read once here: the player notifies its duration with every position
                            readonly property real duration: page.player.durationTicks
                            anchors.fill: parent
                            Kirigami.Theme.colorSet: Kirigami.Theme.Complementary
                            Kirigami.Theme.inherit: false
                            Accessible.ignored: true
                            Repeater {
                                model: page.chapters
                                delegate: Rectangle {
                                    required property var modelData
                                    readonly property real fraction: chapterMarks.duration > 0 ? Math.min(1, modelData.startTicks / chapterMarks.duration) : 0
                                    readonly property real middle: timeline.leftPadding + timeline.handle.width / 2 + (timeline.mirrored ? 1 - fraction : fraction) * (timeline.availableWidth - timeline.handle.width)
                                    objectName: "playerChapterMark"
                                    visible: fraction > 0 && Math.abs(middle - timeline.handle.x - timeline.handle.width / 2) > timeline.handle.width / 2
                                    x: Math.round(middle)
                                    y: Math.round(timeline.topPadding + (timeline.availableHeight - height) / 2)
                                    width: 1
                                    height: Kirigami.Units.smallSpacing * 2
                                    color: Kirigami.Theme.backgroundColor
                                }
                            }
                        }
                    }
                    Controls.ToolButton {
                        id: timeButton
                        objectName: "playerTimeButton"
                        visible: page.player.durationTicks > 0
                        // As wide as its widest text, so neither the button's frame nor the timeline beside it move while the numbers change
                        Layout.preferredWidth: Math.max(implicitWidth, widestTime.implicitWidth)
                        focusPolicy: Qt.TabFocus
                        font.features: { "tnum": 1 }
                        text: page.endTimeLabel
                        Accessible.name: page.showRemaining ? i18nc("@action:button accessible name", "Remaining time %1", page.player.timeText(Math.max(0, page.player.durationTicks - page.player.positionTicks))) : i18nc("@action:button accessible name", "Total time %1", page.player.timeText(page.player.durationTicks))
                        Accessible.description: Controls.ToolTip.text
                        Controls.ToolTip.text: page.showRemaining ? i18nc("@info:tooltip", "Show total time") : i18nc("@info:tooltip", "Show remaining time")
                        Controls.ToolTip.visible: hovered || visualFocus
                        Controls.ToolTip.delay: Kirigami.Units.toolTipDelay
                        onClicked: { page.showRemaining = !page.showRemaining; page.wakeChrome(); }
                    }
                    // The time button with its widest text, never shown: the style paints the text inside margins of its own, so only the style can say how wide that is
                    Controls.ToolButton {
                        id: widestTime
                        visible: false
                        font: timeButton.font
                        text: page.widestEndTimeLabel
                    }
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
                        onClicked: page.skipPrevious()
                    }
                    OsdButton {
                        id: playButton
                        objectName: "playerPauseButton"
                        text: page.pauseText
                        icon.name: page.pauseIcon
                        enabled: page.canTogglePause
                        onClicked: page.togglePlayback()
                    }
                    OsdButton {
                        objectName: "playerNextButton"
                        visible: page.player.hasNext
                        icon.name: "media-skip-forward"
                        text: i18n("Next")
                        onClicked: page.skipNext()
                    }
                    // A click mutes; hover, keyboard focus or a long press show the volume slider; the wheel and, with the focus here, Up and Down change the volume by five
                    OsdButton {
                        id: muteButton
                        objectName: "playerMuteButton"
                        // A device whose volume cannot be set has no volume to show
                        visible: !page.player.casting || page.player.castHasVolume
                        icon.name: page.player.muted || page.player.volume === 0 ? "audio-volume-muted" : page.player.volume < 34 ? "audio-volume-low" : page.player.volume < 67 ? "audio-volume-medium" : "audio-volume-high"
                        text: page.player.muted ? i18n("Unmute") : i18n("Mute")
                        Accessible.description: i18nc("@info volume level", "Volume %1%", page.player.volume)
                        // The volume slider takes the tooltip's place above the button (a deliberate HIG exception, see the architecture notes)
                        Controls.ToolTip.visible: (hovered || visualFocus) && !volumePopup.opened
                        onClicked: { page.player.toggleMute(); page.wakeChrome(); }
                        onPressAndHold: page.showVolume()
                        onHoveredChanged: hovered ? page.showVolume() : volumeCloser.restart()
                        onVisualFocusChanged: visualFocus ? page.showVolume() : volumeCloser.restart()
                        Keys.onUpPressed: event => { page.stepVolume(1); event.accepted = true; }
                        Keys.onDownPressed: event => { page.stepVolume(-1); event.accepted = true; }
                        VolumeWheel {
                            anchors.fill: parent
                            onStepped: steps => page.stepVolume(steps)
                        }
                    }
                    Item { Layout.fillWidth: true }
                    Controls.Label {
                        objectName: "playerEndsAtLabel"
                        // The icon-only row of a narrow window has no room for it
                        visible: text.length > 0 && !page.narrow
                        Layout.rightMargin: Kirigami.Units.largeSpacing
                        textFormat: Text.PlainText
                        text: page.endsAtText
                    }
                    OsdButton {
                        id: audioButton
                        objectName: "playerAudioButton"
                        display: page.narrow ? Controls.AbstractButton.IconOnly : Controls.AbstractButton.TextBesideIcon
                        enabled: page.player.audioTracks.length > 0 && !page.player.casting
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
                        enabled: !page.player.casting
                        onClicked: page.openMenu(subtitlesMenu, subtitlesButton)
                    }
                    OsdButton {
                        objectName: "playerFullScreenButton"
                        icon.name: page.fullScreen ? "view-restore" : "view-fullscreen"
                        text: page.fullScreenText
                        onClicked: page.toggleFullScreen()
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
    // Over the bars and the video, and it stays when the bars go; below the top bar while that is up
    PlaybackInfoOverlay {
        id: playbackInfo
        opacity: page.playbackInfoShown ? 1 : 0
        visible: page.playbackInfoShown || opacity > 0
        Behavior on opacity {
            NumberAnimation { duration: Kirigami.Units.shortDuration; easing.type: Easing.OutCubic }
        }
        player: page.player
        playerSize: page.videoPixelSize
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.leftMargin: Kirigami.Units.largeSpacing
        anchors.topMargin: osd.wanted ? topRow.y + topRow.implicitHeight + Kirigami.Units.largeSpacing : Kirigami.Units.largeSpacing
        width: Math.min(Kirigami.Units.gridUnit * 30, page.width - Kirigami.Units.largeSpacing * 2)
        maximumHeight: page.height - anchors.topMargin - bottomBar.height
        Behavior on anchors.topMargin {
            NumberAnimation { duration: Kirigami.Units.longDuration; easing.type: Easing.OutCubic }
        }
        onCloseRequested: page.closePlaybackInfo()
    }
    // The menus of the bottom bar belong to the page, not to the bars: they keep the window's colours over the dark bars
    AudioMenu {
        id: audioMenu
        objectName: "playerAudioMenu"
        onAboutToHide: page.popupGone()
        onClosed: page.popupClosed()
    }
    SubtitleSearchDialog {
        id: subtitleSearchDialog
        player: page.player
    }
    FileDialog {
        id: clipSaveDialog
        property bool gif: false
        fileMode: FileDialog.SaveFile
        title: gif ? i18n("Save GIF") : i18n("Save Video Clip")
        nameFilters: gif ? [i18n("GIF images (*.gif)")] : [i18n("Video clips (*.mp4 *.webm)")]
        currentFile: page.player.suggestedClipUrl(gif)
        onAccepted: page.player.saveClip(gif, selectedFile)
    }

    Connections {
        target: page.player
        // The shell tests load the page without a player
        ignoreUnknownSignals: true
        function onRemoteSubtitleDownloaded(subtitleId) {
            page.Window.window?.showPassiveNotification(i18n("Subtitle downloaded"));
        }
        function onRemoteSubtitleDownloadFailed(subtitleId, httpStatus) {
            page.Window.window?.showPassiveNotification(i18n("The subtitle could not be downloaded: check the connection and try again"));
        }
        function onRemoteSubtitleNotSaved(subtitleId) {
            page.Window.window?.showPassiveNotification(i18n("The server could not save the subtitle: its administrator can check where the library stores subtitles"));
        }
        function onNightModePassthroughDisabled() {
            page.Window.window?.showPassiveNotification(i18n("Night mode is on: passthrough is off until it is turned off"));
        }
    }

    SubtitlesMenu {
        id: subtitlesMenu
        objectName: "playerSubtitlesMenu"
        onAboutToHide: page.popupGone()
        onClosed: page.popupClosed()
    }
    MoreMenu {
        id: moreMenu
        objectName: "playerMoreMenu"
        onAboutToHide: page.popupGone()
        onClosed: page.popupClosed()
        leading: [
            Controls.MenuItem {
                objectName: "playerMoreMiniItem"
                icon.name: "window-minimize-pip"
                text: i18nc("@action:button", "Show Mini Player")
                onTriggered: page.leavePlayer()
            },
            Controls.MenuSeparator {}
        ]
    }
    // The right click on the video: the controls of the bars, then the longer More Actions list as one submenu so the menu stays short
    Controls.Menu {
        id: contextMenu
        objectName: "playerContextMenu"
        onAboutToHide: page.popupGone()
        onClosed: page.popupClosed()

        // The buttons the page shows over the video are reachable by Tab only: the entries come while their button shows, so the Menu key and a pad's West button reach them
        Controls.MenuItem {
            objectName: "playerContextSkipItem"
            visible: skipButton.shown
            height: visible ? implicitHeight : 0
            icon.name: "media-skip-forward"
            text: skipButton.shownLabel
            onTriggered: { page.player.skipSegment(); page.wakeChrome(); }
        }
        Controls.MenuItem {
            objectName: "playerContextRetryItem"
            visible: !!page.player.errorText && !!page.player.canRetry
            height: visible ? implicitHeight : 0
            icon.name: "view-refresh"
            text: i18nc("@action:inmenu", "Try Again")
            onTriggered: page.player.retryPlayback()
        }
        Controls.MenuItem {
            objectName: "playerContextCloseItem"
            visible: !!page.player.errorText
            height: visible ? implicitHeight : 0
            icon.name: "window-close"
            text: i18nc("@action:inmenu", "Close Player")
            onTriggered: page.closePlayer()
        }
        Controls.MenuItem {
            objectName: "playerContextCancelClipItem"
            visible: page.player.clipSaving
            height: visible ? implicitHeight : 0
            icon.name: "dialog-cancel"
            text: page.player.clipSavingGif ? i18nc("@action:inmenu", "Cancel Saving the GIF") : i18nc("@action:inmenu", "Cancel Saving the Clip")
            onTriggered: page.player.cancelClip()
        }
        Controls.MenuItem {
            objectName: "playerContextSyncPlayItem"
            visible: page.grouped
            height: visible ? implicitHeight : 0
            icon.name: "system-users"
            text: i18nc("@action:inmenu", "Show SyncPlay Group")
            onTriggered: page.Window.window?.openSyncPlay()
        }
        Controls.MenuSeparator {
            visible: skipButton.shown || !!page.player.errorText || page.player.clipSaving || page.grouped
            height: visible ? implicitHeight : 0
        }
        Controls.MenuItem {
            objectName: "playerContextPauseItem"
            text: page.pauseText
            icon.name: page.pauseIcon
            enabled: page.canTogglePause
            onTriggered: page.togglePlayback()
        }
        Controls.MenuItem {
            objectName: "playerContextPreviousItem"
            // A menu keeps its entries in place: an entry with nothing to do is disabled, not hidden
            enabled: page.player.hasPrevious
            icon.name: "media-skip-backward"
            text: i18n("Previous")
            onTriggered: page.skipPrevious()
        }
        Controls.MenuItem {
            objectName: "playerContextNextItem"
            enabled: page.player.hasNext
            icon.name: "media-skip-forward"
            text: i18n("Next")
            onTriggered: page.skipNext()
        }
        Controls.MenuSeparator {}
        AudioMenu {
            objectName: "playerContextAudioMenu"
            namePrefix: "playerContext"
            enabled: page.player.audioTracks.length > 0 && !page.player.casting
            title: i18nc("@action:button audio tracks", "Audio")
            icon.name: "audio-headphones"
        }
        SubtitlesMenu {
            objectName: "playerContextSubtitlesMenu"
            namePrefix: "playerContext"
            enabled: !page.player.casting
            title: i18nc("@action:button subtitle tracks", "Subtitles")
            icon.name: "media-view-subtitles-symbolic"
        }
        Controls.MenuSeparator {}
        Controls.MenuItem {
            objectName: "playerContextFullScreenItem"
            icon.name: page.fullScreen ? "view-restore" : "view-fullscreen"
            text: page.fullScreenText
            onTriggered: page.toggleFullScreen()
        }
        MoreMenu {
            id: contextMoreMenu
            objectName: "playerContextMoreMenu"
            namePrefix: "playerContextMore"
            title: i18n("More Actions")
            icon.name: "overflow-menu"
        }
    }
    Controls.Popup {
        id: volumePopup
        objectName: "playerVolumePopup"
        // Above the speaker and centred on it, whatever the level label's width or the window does
        parent: muteButton
        x: Math.round((muteButton.width - width) / 2)
        y: -height - Kirigami.Units.smallSpacing
        // The keyboard stays with the speaker button and the page; a press on the speaker mutes and keeps the slider. No CloseOnEscape: its Escape shortcut would make the window's ambiguous and neither would fire, so the page's back closes it
        focus: false
        modal: false
        closePolicy: Controls.Popup.CloseOnPressOutsideParent
        padding: Kirigami.Units.smallSpacing
        // Only the way out fades: an enter transition would hold `opened` back, which the page's Escape and hover rules read; the exit leaves the opacity at 0, so each show puts it back
        onAboutToShow: opacity = 1
        exit: Transition {
            NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Kirigami.Units.shortDuration; easing.type: Easing.InCubic }
        }
        // The window's colours, like the menus, not the dark bars' its parent has
        Kirigami.Theme.colorSet: Kirigami.Theme.Window
        Kirigami.Theme.inherit: false
        // The bars hiding close it without waking them again
        onAboutToHide: page.popupClosedAt = Date.now()
        onClosed: if (page.showChrome) chromeTimer.restart()
        contentItem: Item {
            implicitWidth: volumeColumn.implicitWidth
            implicitHeight: volumeColumn.implicitHeight
            HoverHandler {
                id: volumeHover
                onHoveredChanged: if (!hovered) volumeCloser.restart()
            }
            ColumnLayout {
                id: volumeColumn
                anchors.fill: parent
                spacing: Kirigami.Units.smallSpacing
                // As wide as the widest level, so the popup keeps its width while the level changes
                Controls.Label {
                    id: volumeLevel
                    Layout.alignment: Qt.AlignHCenter
                    Layout.preferredWidth: Math.max(implicitWidth, widestVolume.implicitWidth)
                    horizontalAlignment: Text.AlignHCenter
                    font.features: { "tnum": 1 }
                    text: i18nc("@label volume level", "%1%", page.player.volume)
                }
                // The level label with the widest level, never shown, measured the way the label renders
                Controls.Label {
                    id: widestVolume
                    visible: false
                    font: volumeLevel.font
                    text: i18nc("@label volume level", "%1%", 100)
                }
                Controls.Slider {
                    id: volumeSlider
                    objectName: "playerVolumeSlider"
                    Layout.alignment: Qt.AlignHCenter
                    Layout.preferredHeight: Kirigami.Units.gridUnit * 7
                    orientation: Qt.Vertical
                    focusPolicy: Qt.NoFocus
                    from: 0
                    to: 100
                    // The level moves by five; the style would draw a tick mark at every step
                    stepSize: 5
                    Kirigami.StyleHints.tickMarkStepSize: -1
                    value: page.player.volume
                    onMoved: { page.player.setVolume(Math.round(value)); page.wakeChrome(); }
                    onPressedChanged: if (!pressed) volumeCloser.restart()
                    Accessible.name: i18n("Volume")
                }
            }
            // Above the slider for the wheel only: presses go through to it
            VolumeWheel {
                anchors.fill: parent
                onStepped: steps => page.stepVolume(steps)
            }
        }
    }
    // The slider goes a moment after neither the pointer, the keyboard nor a drag holds it any more
    Timer {
        id: volumeCloser
        interval: 400
        onTriggered: {
            if (!muteButton.hovered && !muteButton.visualFocus && !volumeHover.hovered && !volumeSlider.pressed)
                volumePopup.close();
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
        // Only the way out fades: an enter transition would hold `opened` back, which the page's Escape and hover rules read; the exit leaves the opacity at 0, so each show puts it back
        onAboutToShow: opacity = 1
        exit: Transition {
            NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Kirigami.Units.shortDuration; easing.type: Easing.InCubic }
        }
        // The hide timer gives way while the panel is open: it starts again when the panel goes
        onAboutToHide: page.popupGone()
        onClosed: page.popupClosed()
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

    // A click on a bar, beside its controls, is not a click on the video: it neither pauses nor toggles full screen; the user's handler wakes the bars
    component BarClickGuard: MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
    }
    // The wheel over the speaker and its slider: five per notch, a touchpad's travel added up to whole notches
    component VolumeWheel: MouseArea {
        id: wheelArea
        signal stepped(int steps)
        property real travel: 0
        property double lastWheel: 0
        acceptedButtons: Qt.NoButton
        onWheel: wheel => {
            // A half notch left from a pause ago or from the other direction does not count
            const now = Date.now();
            if (now - wheelArea.lastWheel > 500 || wheelArea.travel * wheel.angleDelta.y < 0)
                wheelArea.travel = 0;
            wheelArea.lastWheel = now;
            wheelArea.travel += wheel.angleDelta.y;
            while (Math.abs(wheelArea.travel) >= 120) {
                const up = wheelArea.travel > 0;
                wheelArea.travel -= up ? 120 : -120;
                wheelArea.stepped(up ? 1 : -1);
            }
        }
    }
    // A menu entry for a name from the server or the file: it never becomes the entry's `text`, which the style's label reads as markup (it is built even when replaced) and searches for a mnemonic. The space before the name is the style's for a check mark, which every menu that uses it has
    component PlainMenuItem: Controls.MenuItem {
        id: plainItem
        property string plainText
        Accessible.name: plainText
        contentItem: RowLayout {
            Item {
                Layout.preferredWidth: plainItem.indicator ? plainItem.indicator.width : Kirigami.Units.smallSpacing
            }
            Controls.Label {
                Layout.fillWidth: true
                Layout.maximumWidth: Kirigami.Units.gridUnit * 24
                Layout.alignment: Qt.AlignVCenter
                text: plainItem.plainText
                textFormat: Text.PlainText
                maximumLineCount: 1
                font: plainItem.font
                color: plainItem.pressed ? Kirigami.Theme.highlightedTextColor : Kirigami.Theme.textColor
                elide: Text.ElideRight
            }
        }
    }
    // The entries of the Audio menu: the bottom bar's menu and the context menu's submenu are both one of these
    component AudioMenu: Controls.Menu {
        id: self
        // The prefix of the entries' object names, so a second instance has names of its own
        property string namePrefix: "player"
        Instantiator {
            model: page.player.audioTracks
            delegate: PlainMenuItem {
                plainText: modelData.name
                checkable: true
                autoExclusive: true
                checked: modelData.selected
                onTriggered: page.player.selectAudioTrack(modelData.id)
            }
            onObjectAdded: (index, object) => self.insertItem(index, object)
            onObjectRemoved: (index, object) => self.removeItem(object)
        }
        Controls.MenuSeparator {}
        Controls.MenuItem {
            objectName: self.namePrefix + "NightMode"
            text: i18n("Night Mode")
            checkable: true
            checked: page.player.nightMode
            onTriggered: page.player.setNightMode(checked)
        }
    }
    // The entries of the Subtitles menu, likewise
    component SubtitlesMenu: Controls.Menu {
        id: self
        property string namePrefix: "player"
        Controls.MenuItem {
            objectName: self.namePrefix + "SubtitlesOff"
            text: i18nc("@item:inmenu no subtitles", "Off")
            checkable: true
            autoExclusive: true
            checked: !page.player.subtitleTracks.some(track => track.selected)
            onTriggered: page.player.selectSubtitleTrack(0)
        }
        Instantiator {
            model: page.player.subtitleTracks
            delegate: PlainMenuItem {
                plainText: modelData.name
                checkable: true
                autoExclusive: true
                checked: modelData.selected
                onTriggered: page.player.selectSubtitleTrack(modelData.id)
            }
            onObjectAdded: (index, object) => self.insertItem(index + 1, object)
            onObjectRemoved: (index, object) => self.removeItem(object)
        }
        // mpv's second subtitle slot: a text track other than the primary
        Controls.Menu {
            id: secondarySubtitlesMenu
            objectName: self.namePrefix + "SecondarySubtitlesMenu"
            title: i18n("Secondary Subtitles")
            enabled: page.player.subtitleTracks.some(track => track.text && !track.selected)
            Controls.MenuItem {
                objectName: self.namePrefix + "SecondarySubtitlesOff"
                text: i18nc("@item:inmenu no second subtitle", "Off")
                checkable: true
                autoExclusive: true
                checked: !page.player.subtitleTracks.some(track => track.secondary)
                onTriggered: page.player.selectSecondarySubtitleTrack(0)
            }
            Instantiator {
                model: page.player.subtitleTracks
                delegate: PlainMenuItem {
                    visible: Boolean(modelData.text && !modelData.selected)
                    plainText: modelData.name
                    checkable: true
                    autoExclusive: true
                    checked: Boolean(modelData.secondary)
                    onTriggered: page.player.selectSecondarySubtitleTrack(modelData.id)
                }
                onObjectAdded: (index, object) =>
                                secondarySubtitlesMenu.insertItem(index + 1, object)
                onObjectRemoved: (index, object) => secondarySubtitlesMenu.removeItem(object)
            }
        }
        Controls.MenuSeparator { visible: page.player.canManageSubtitles }
        Controls.MenuItem {
            objectName: self.namePrefix + "SearchOnline"
            text: i18n("Search Online Subtitles…")
            icon.name: "search"
            visible: page.player.canManageSubtitles && !page.player.localPlayback
            enabled: !page.player.casting
            onTriggered: subtitleSearchDialog.open()
        }
        Controls.MenuSeparator { visible: page.player.externalSubtitles.length > 0 }
        Controls.MenuItem {
            text: i18n("Download External Subtitles")
            enabled: false
            visible: page.player.externalSubtitles.length > 0
        }
        Instantiator {
            model: page.player.externalSubtitles
            delegate: PlainMenuItem {
                plainText: modelData.name
                onTriggered: page.player.downloadSubtitle(modelData.index)
            }
            onObjectAdded: (index, object) => self.addItem(object)
            onObjectRemoved: (index, object) => self.removeItem(object)
        }
    }
    // The entries of More Actions, likewise; a second instance adds its own `leading` entries in front
    component MoreMenu: Controls.Menu {
        id: moreRoot
        // The prefix of the entries' object names, so a second instance has names of its own
        property string namePrefix: "player"
        // Entries that come before the common ones, in order
        property list<QtObject> leading
        Component.onCompleted: {
            for (let i = 0; i < leading.length; ++i) {
                if (typeof leading[i].addMenu === "function")
                    insertMenu(i, leading[i]);
                else
                    insertItem(i, leading[i]);
            }
        }
        Controls.MenuItem {
            objectName: moreRoot.namePrefix + "ShuffleItem"
            text: i18n("Shuffle")
            icon.name: "media-playlist-shuffle"
            checkable: true
            checked: page.player.shuffle
            // A series started from one of its episodes plays in order: only a queue shuffles, and a shuffle that is on can always be turned off; in a group it is the group's queue
            enabled: page.groupFollowing ? page.syncPlay.queue.count > 1 || page.player.shuffle : page.player.queueCount > 1 || page.player.shuffle
            onTriggered: page.player.setShuffle(!page.player.shuffle)
        }
        Controls.Menu {
            objectName: moreRoot.namePrefix + "RepeatMenu"
            title: i18n("Repeat")
            icon.name: "media-playlist-repeat"
            Controls.MenuItem {
                objectName: moreRoot.namePrefix + "RepeatOff"
                text: i18nc("@item:inmenu repeat", "Off")
                checkable: true
                autoExclusive: true
                checked: page.player.repeatMode === 0
                onTriggered: page.player.setRepeatMode(0)
            }
            Controls.MenuItem {
                objectName: moreRoot.namePrefix + "RepeatAll"
                text: i18nc("@item:inmenu repeat", "All")
                // A lone item repeats as the current item only; a repeat of all that is on can always be turned off
                enabled: page.groupFollowing || page.player.queueCount > 0 || page.player.hasNext || page.player.hasPrevious || page.player.repeatMode === 1
                checkable: true
                autoExclusive: true
                checked: page.player.repeatMode === 1
                onTriggered: page.player.setRepeatMode(1)
            }
            Controls.MenuItem {
                objectName: moreRoot.namePrefix + "RepeatOne"
                text: i18nc("@item:inmenu repeat", "Current Item")
                checkable: true
                autoExclusive: true
                checked: page.player.repeatMode === 2
                onTriggered: page.player.setRepeatMode(2)
            }
        }
        Controls.Menu {
            id: speedMenu
            objectName: moreRoot.namePrefix + "SpeedMenu"
            // A group plays at its own pace, and a cast has no local rate
            enabled: !page.groupFollowing && !page.player.casting
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
        // This playback only
        Controls.Menu {
            objectName: moreRoot.namePrefix + "AspectMenu"
            title: i18nc("@title:menu", "Aspect Ratio")
            icon.name: "zoom-fit-best"
            enabled: !page.player.casting
            AspectItem { namePrefix: moreRoot.namePrefix; mode: "normal"; text: i18nc("@item:inmenu aspect ratio", "Auto") }
            AspectItem { namePrefix: moreRoot.namePrefix; mode: "zoom"; text: i18nc("@item:inmenu aspect ratio", "Zoom") }
            AspectItem { namePrefix: moreRoot.namePrefix; mode: "stretch"; text: i18nc("@item:inmenu aspect ratio", "Stretch") }
            AspectItem { namePrefix: moreRoot.namePrefix; mode: "force_4_3"; text: i18nc("@item:inmenu aspect ratio", "4:3") }
            AspectItem { namePrefix: moreRoot.namePrefix; mode: "force_16_9"; text: i18nc("@item:inmenu aspect ratio", "16:9") }
            AspectItem { namePrefix: moreRoot.namePrefix; mode: "force_16_9_if_4_3"; text: i18nc("@item:inmenu aspect ratio", "Widescreen for 4:3 Video") }
            AspectItem { namePrefix: moreRoot.namePrefix; mode: "noscaling"; text: i18nc("@item:inmenu aspect ratio", "Original Size") }
        }
        // A server max bit rate for this playback; disabled for local files, groups and without the transcode policy
        Controls.Menu {
            id: qualityMenu
            objectName: moreRoot.namePrefix + "QualityMenu"
            title: i18n("Quality")
            icon.name: "speedometer"
            enabled: page.player.qualityAvailable && !page.player.qualitySwitching && !page.player.casting
            Instantiator {
                model: page.player.qualitySteps
                delegate: PlainMenuItem {
                    plainText: modelData.text
                    checkable: true
                    autoExclusive: true
                    checked: modelData.checked
                    onTriggered: page.player.setQuality(modelData.cap)
                }
                onObjectAdded: (index, object) => qualityMenu.insertItem(index, object)
                onObjectRemoved: (index, object) => qualityMenu.removeItem(object)
            }
        }
        // Disabled rather than hidden when the item has none
        Controls.Menu {
            id: chaptersMenu
            objectName: moreRoot.namePrefix + "ChaptersMenu"
            title: i18nc("@title:menu", "Chapters")
            icon.name: "bookmarks"
            enabled: page.chapters.length > 0
            Instantiator {
                model: page.chapters
                delegate: PlainMenuItem {
                    required property var modelData
                    required property int index
                    objectName: moreRoot.namePrefix + "Chapter_" + index
                    plainText: i18nc("@item:inmenu chapter, %1 is its start time and %2 its name", "%1 — %2", modelData.time, modelData.name)
                    checkable: true
                    autoExclusive: true
                    checked: page.player.currentChapter === index
                    onTriggered: page.player.jumpToChapter(index)
                }
                onObjectAdded: (index, object) => chaptersMenu.insertItem(index, object)
                onObjectRemoved: (index, object) => chaptersMenu.removeItem(object)
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
            objectName: "playerSleepMenu"
            title: page.player.sleepAtEndOfItem ? i18n("Sleep: End of Current Item")
                 : page.player.sleepAtEndOfQueue ? i18n("Sleep: End of Queue")
                 : page.player.sleepRemainingMinutes > 0 ? i18n("Sleep: %1 min", page.player.sleepRemainingMinutes)
                 : i18n("Sleep Timer")
            icon.name: "chronometer"
            Controls.MenuItem {
                text: i18n("Off")
                checkable: true
                autoExclusive: true
                checked: !page.player.sleepAtEndOfItem && !page.player.sleepAtEndOfQueue && page.player.sleepRemainingMinutes === 0
                onTriggered: page.player.setSleepTimer(0)
            }
            Controls.MenuItem {
                text: i18n("End of Current Item")
                checkable: true
                autoExclusive: true
                checked: page.player.sleepAtEndOfItem
                onTriggered: page.player.setSleepAtEndOfItem(true)
            }
            Controls.MenuItem {
                text: i18n("End of Queue")
                checkable: true
                autoExclusive: true
                checked: page.player.sleepAtEndOfQueue
                onTriggered: page.player.setSleepAtEndOfQueue(true)
            }
            Controls.MenuSeparator {}
            Instantiator {
                model: [15, 30, 45, 60, 90, 120]
                delegate: Controls.MenuItem {
                    required property int modelData
                    text: i18np("%1 minute", "%1 minutes", modelData)
                    onTriggered: page.player.setSleepTimer(modelData)
                }
                // After the fixed entries and the separator
                onObjectAdded: (index, object) => sleepMenu.addItem(object)
                onObjectRemoved: (index, object) => sleepMenu.removeItem(object)
            }
        }
        // The tracks, speed and aspect the next episodes of this series would start with
        Controls.MenuItem {
            objectName: moreRoot.namePrefix + "ForgetSeriesItem"
            text: i18nc("@action:inmenu", "Forget Series Choices")
            icon.name: "edit-clear-history"
            enabled: page.player.hasSeriesChoices
            onTriggered: page.player.forgetSeriesChoices()
        }
        Controls.MenuSeparator {}
        Controls.MenuItem {
            objectName: moreRoot.namePrefix + "QueueItem"
            text: i18n("Now Playing Queue")
            icon.name: "media-track-queue-amarok"
            visible: !page.player.localPlayback
            onTriggered: queuePanel.open()
        }
        Controls.MenuItem {
            text: i18n("Screenshot")
            // The full-colour camera-photo of breeze has path data Qt's SVG renderer truncates, with a warning on every draw
            icon.name: "camera-photo-symbolic"
            enabled: !page.player.casting
            onTriggered: {
                page.player.screenshot();
                page.wakeChrome();
                screenshotNotice.restart();
            }
        }
        Controls.Menu {
            id: clipMenu
            objectName: moreRoot.namePrefix + "ClipMenu"
            title: i18n("Clip")
            icon.name: "media-record"
            enabled: !page.player.casting
            Controls.MenuItem {
                objectName: moreRoot.namePrefix + "ClipStart"
                text: i18n("Set Start")
                enabled: page.player.active
                onTriggered: page.player.markClipStart()
            }
            Controls.MenuItem {
                objectName: moreRoot.namePrefix + "ClipEnd"
                text: i18n("Set End")
                enabled: page.player.active
                onTriggered: page.player.markClipEnd()
            }
            Controls.MenuSeparator {}
            // The marks in words, then the reason neither Save entry can be used
            Controls.MenuItem {
                objectName: moreRoot.namePrefix + "ClipLine"
                text: page.player.clipLine
                visible: text.length > 0
                enabled: false
            }
            Controls.MenuItem {
                objectName: moreRoot.namePrefix + "ClipReason"
                text: page.player.clipReason
                visible: text.length > 0
                enabled: false
            }
            Controls.MenuItem {
                objectName: moreRoot.namePrefix + "ClipLengthReason"
                text: page.player.clipLengthReason
                visible: text.length > 0
                enabled: false
            }
            Controls.MenuItem {
                objectName: moreRoot.namePrefix + "ClipSaveVideo"
                text: i18n("Save Video Clip…")
                enabled: page.player.clipVideoEnabled
                onTriggered: {
                    clipSaveDialog.gif = false;
                    clipSaveDialog.open();
                }
            }
            Controls.MenuItem {
                objectName: moreRoot.namePrefix + "ClipSaveGif"
                text: i18n("Save GIF…")
                enabled: page.player.clipGifEnabled
                onTriggered: {
                    clipSaveDialog.gif = true;
                    clipSaveDialog.open();
                }
            }
            Controls.MenuSeparator {}
            Controls.MenuItem {
                objectName: moreRoot.namePrefix + "ClipClear"
                text: i18n("Clear Clip")
                enabled: page.player.clipStartTicks >= 0 || page.player.clipEndTicks >= 0
                onTriggered: page.player.clearClip()
            }
        }
        Controls.MenuItem {
            objectName: moreRoot.namePrefix + "PlaybackInfoItem"
            text: i18nc("@action:inmenu", "Playback Info")
            icon.name: "dialog-information"
            visible: !page.player.localPlayback
            checkable: true
            checked: page.playbackInfoShown
            onTriggered: page.playbackInfoShown = !page.playbackInfoShown
        }
        Controls.MenuSeparator {}
        Controls.MenuItem {
            objectName: moreRoot.namePrefix + "LockItem"
            icon.name: "object-locked"
            text: i18nc("@action:inmenu", "Keep Controls Visible")
            checkable: true
            checked: page.osdLocked
            onTriggered: page.osdLocked = !page.osdLocked
        }
    }
    // A checkable entry of the Aspect Ratio menu: it follows the player's mode and sets it
    component AspectItem: Controls.MenuItem {
        required property string mode
        property string namePrefix: "player"
        objectName: namePrefix + "Aspect_" + mode
        checkable: true
        autoExclusive: true
        checked: page.player.aspectMode === mode
        onTriggered: page.player.setAspectMode(mode)
    }
}
