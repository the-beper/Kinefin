// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// How the item plays, what the server transcodes and what the file is, over the video
Rectangle {
    id: overlay
    objectName: "playbackInfoOverlay"
    required property var player
    // The video item's size in pixels
    property size playerSize: Qt.size(0, 0)
    // The panel scrolls when its rows are taller than this
    property real maximumHeight: Kirigami.Units.gridUnit * 40
    // The last answer of the player's playbackInfo()
    property var info: ({})
    property string encodedInfo
    readonly property var sections: overlay.buildSections(overlay.info)
    // The size of the player and of the video in pixels, kept out of the sections so a resize changes one label and rebuilds no rows
    readonly property string sizesText: overlay.buildSizes(overlay.info, overlay.playerSize)
    signal closeRequested()

    implicitHeight: flick.contentHeight + Kirigami.Units.largeSpacing * 2
    height: Math.max(0, Math.min(implicitHeight, maximumHeight))
    radius: Kirigami.Units.smallSpacing
    color: Qt.alpha(Kirigami.Theme.backgroundColor, 0.85)
    Kirigami.Theme.colorSet: Kirigami.Theme.Complementary
    Kirigami.Theme.inherit: false
    Accessible.role: Accessible.Pane
    Accessible.name: i18nc("@title:window accessible name of the panel", "Playback Info")

    // The rows are rebuilt only when the answer changed, and the scroll position stays through it
    function refresh() {
        const answer = overlay.player.playbackInfo();
        const encoded = JSON.stringify(answer);
        if (encoded === overlay.encodedInfo)
            return;
        overlay.encodedInfo = encoded;
        const scrolled = flick.contentY;
        overlay.info = answer;
        flick.contentY = scrolled;
    }
    Timer {
        objectName: "playbackInfoTimer"
        interval: 1000
        repeat: true
        running: overlay.visible
        triggeredOnStart: true
        onTriggered: overlay.refresh()
    }

    function present(value) {
        return value !== undefined && value !== null && String(value).length > 0;
    }
    // The decimals a value needs, up to three
    function fractionDigits(value) {
        for (let digits = 0; digits < 3; ++digits) {
            if (Math.abs(value - Number(value.toFixed(digits))) < 0.0005)
                return digits;
        }
        return 3;
    }
    function number(value, digits) {
        return Number(value).toLocaleString(Qt.locale(), "f", digits);
    }
    function withDetail(value, detail) {
        return i18nc("@item:intext a value with its detail in brackets", "%1 (%2)", value, detail);
    }
    function megabits(bitsPerSecond) {
        return i18nc("@item:intext bitrate", "%1 Mbps", overlay.number(bitsPerSecond / 1000000, 1));
    }
    function sizeText(bytes) {
        const mebibyte = 1024 * 1024;
        if (bytes >= 1024 * mebibyte)
            return i18nc("@item:intext file size", "%1 GiB", overlay.number(bytes / (1024 * mebibyte), 2));
        return i18nc("@item:intext file size", "%1 MiB", overlay.number(bytes / mebibyte, 1));
    }
    function methodText(method) {
        switch (method) {
        case "Direct":
        case "DirectPlay": return i18nc("@item:intext how the item plays", "Direct Play");
        case "Transcode": return i18nc("@item:intext how the item plays", "Transcoding");
        default: return "";
        }
    }
    // The sentence of a reason the server gives for transcoding, empty for a name this app does not know
    function reasonText(name) {
        switch (name) {
        case "ContainerNotSupported": return i18nc("@item:intext transcoding reason", "The container is not supported");
        case "VideoCodecNotSupported": return i18nc("@item:intext transcoding reason", "The video codec is not supported");
        case "AudioCodecNotSupported": return i18nc("@item:intext transcoding reason", "The audio codec is not supported");
        case "SubtitleCodecNotSupported": return i18nc("@item:intext transcoding reason", "The subtitle codec is not supported");
        case "AudioIsExternal": return i18nc("@item:intext transcoding reason", "The audio is in an external file");
        case "SecondaryAudioNotSupported": return i18nc("@item:intext transcoding reason", "Secondary audio is not supported");
        case "VideoProfileNotSupported": return i18nc("@item:intext transcoding reason", "The video profile is not supported");
        case "VideoLevelNotSupported": return i18nc("@item:intext transcoding reason", "The video level is not supported");
        case "VideoResolutionNotSupported": return i18nc("@item:intext transcoding reason", "The video resolution is not supported");
        case "VideoBitDepthNotSupported": return i18nc("@item:intext transcoding reason", "The video bit depth is not supported");
        case "VideoFramerateNotSupported": return i18nc("@item:intext transcoding reason", "The video frame rate is not supported");
        case "RefFramesNotSupported": return i18nc("@item:intext transcoding reason", "The number of reference frames is not supported");
        case "AnamorphicVideoNotSupported": return i18nc("@item:intext transcoding reason", "Anamorphic video is not supported");
        case "InterlacedVideoNotSupported": return i18nc("@item:intext transcoding reason", "Interlaced video is not supported");
        case "AudioChannelsNotSupported": return i18nc("@item:intext transcoding reason", "The number of audio channels is not supported");
        case "AudioProfileNotSupported": return i18nc("@item:intext transcoding reason", "The audio profile is not supported");
        case "AudioSampleRateNotSupported": return i18nc("@item:intext transcoding reason", "The audio sample rate is not supported");
        case "AudioBitDepthNotSupported": return i18nc("@item:intext transcoding reason", "The audio bit depth is not supported");
        case "ContainerBitrateExceedsLimit": return i18nc("@item:intext transcoding reason", "The container bitrate exceeds the limit");
        case "VideoBitrateNotSupported": return i18nc("@item:intext transcoding reason", "The video bitrate is not supported");
        case "AudioBitrateNotSupported": return i18nc("@item:intext transcoding reason", "The audio bitrate is not supported");
        case "UnknownVideoStreamInfo": return i18nc("@item:intext transcoding reason", "The video stream information is unknown");
        case "UnknownAudioStreamInfo": return i18nc("@item:intext transcoding reason", "The audio stream information is unknown");
        case "DirectPlayError": return i18nc("@item:intext transcoding reason", "Direct Play failed");
        case "VideoRangeTypeNotSupported": return i18nc("@item:intext transcoding reason", "The video range type is not supported");
        case "VideoCodecTagNotSupported": return i18nc("@item:intext transcoding reason", "The video codec tag is not supported");
        case "StreamCountExceedsLimit": return i18nc("@item:intext transcoding reason", "The number of streams exceeds the limit");
        default: return "";
        }
    }

    function buildSizes(info, playerSize) {
        const playerPixels = playerSize.width > 0 && playerSize.height > 0 ? playerSize.width + "×" + playerSize.height : "";
        const videoPixels = Number(info.videoWidth) > 0 && Number(info.videoHeight) > 0 ? info.videoWidth + "×" + info.videoHeight : "";
        return playerPixels.length > 0 && videoPixels.length > 0 ? i18nc("@item:intext the size of the player, then the size of the video", "%1 / %2", playerPixels, videoPixels) : playerPixels + videoPixels;
    }

    // The sections of the panel, each with its rows (key, label, text); a row or a section with nothing to show is left out, and a live row takes its text from a property of its own
    function buildSections(info) {
        const sections = [];
        // Nothing is known while the item opens or restarts
        if (Object.keys(info).length === 0)
            return sections;
        const addSection = (key, title, rows) => {
            const shown = rows.filter(row => row.text.length > 0 || row.live === true);
            if (shown.length > 0)
                sections.push({ key: key, title: title, rows: shown });
        };
        const row = (key, label, parts) => ({ key: key, label: label, text: parts.filter(part => part.length > 0).join(" · ") });
        const text = value => overlay.present(value) ? String(value) : "";
        const positive = value => Number(value) > 0;

        const method = overlay.methodText(info.playMethod);
        const stream = overlay.present(info.streamType) && overlay.present(info.protocol) ? overlay.withDetail(info.streamType, info.protocol) : text(info.streamType) + text(info.protocol);
        const hwdec = text(info.hwdec);
        addSection("playback", i18nc("@title:group", "Playback Info"), [
            row("player", i18nc("@label", "Player"), [method.length > 0 ? overlay.withDetail("mpv", method) : "mpv"]),
            row("streamType", i18nc("@label", "Stream type"), [stream]),
            { key: "sizes", label: i18nc("@label", "Player sizes"), text: "", live: true },
            row("hwdec", i18nc("@label", "Hardware decoding"), [hwdec === "no" ? i18nc("@item:intext hardware decoding is off", "Off") : hwdec]),
            row("dropped", i18nc("@label", "Frames dropped"), [overlay.present(info.droppedFrames) ? overlay.number(info.droppedFrames, 0) : ""]),
            row("decoderDropped", i18nc("@label", "Frames dropped by the decoder"), [overlay.present(info.decoderDroppedFrames) ? overlay.number(info.decoderDroppedFrames, 0) : ""])
        ]);

        if (info.playMethod === "Transcode") {
            const reasons = Array.from(info.transcodeReasons ?? []).map(name => overlay.reasonText(String(name))).filter(sentence => sentence.length > 0);
            addSection("transcoding", i18nc("@title:group", "Transcoding Info"), [
                { key: "targetCodecs", label: i18nc("@label", "Target codecs"), text: [text(info.targetVideoCodec), text(info.targetAudioCodec)].filter(codec => codec.length > 0).join(", ") },
                { key: "reasons", label: i18nc("@label", "Reasons"), text: reasons.join("\n") }
            ]);
        }

        addSection("media", i18nc("@title:group", "Original Media Info"), [
            row("container", i18nc("@label", "Container"), [text(info.container), positive(info.sizeBytes) ? overlay.sizeText(Number(info.sizeBytes)) : "", positive(info.bitrate) ? overlay.megabits(Number(info.bitrate)) : ""]),
            row("videoCodec", i18nc("@label", "Video codec"), [text(info.videoCodec)]),
            row("videoAttributes", i18nc("@label", "Video attributes"), [
                text(info.videoProfile),
                overlay.present(info.videoLevel) ? i18nc("@item:intext video level", "Lv %1", info.videoLevel) : "",
                positive(info.videoBitrate) ? overlay.megabits(Number(info.videoBitrate)) : "",
                positive(info.videoFps) ? i18nc("@item:intext frames per second", "%1 fps", overlay.number(info.videoFps, overlay.fractionDigits(Number(info.videoFps)))) : ""
            ]),
            row("videoBitDepth", i18nc("@label", "Video bit depth"), [
                positive(info.videoBitDepth) ? i18nc("@item:intext bits per colour channel", "%1 bit", overlay.number(info.videoBitDepth, 0)) : "",
                text(info.videoPixelFormat),
                text(info.videoRange)
            ]),
            row("videoColors", i18nc("@label", "Video colors"), [text(info.colorSpace), text(info.colorPrimaries), text(info.colorTransfer)]),
            row("audioCodec", i18nc("@label", "Audio codec"), [text(info.audioCodec)]),
            row("audioAttributes", i18nc("@label", "Audio attributes"), [
                positive(info.audioChannels) ? i18nc("@item:intext number of audio channels", "%1 Ch", overlay.number(info.audioChannels, 0)) : "",
                positive(info.audioBitrate) ? i18nc("@item:intext audio bitrate", "%1 kbps", overlay.number(Number(info.audioBitrate) / 1000, 0)) : "",
                positive(info.audioSampleRate) ? i18nc("@item:intext audio sample rate", "%1 Hz", overlay.number(info.audioSampleRate, 0)) : ""
            ])
        ]);
        return sections;
    }

    // A click on the panel, beside its controls, is not a click on the video
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
    }
    Flickable {
        id: flick
        anchors.fill: parent
        anchors.margins: Kirigami.Units.largeSpacing
        contentWidth: width
        contentHeight: column.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        Controls.ScrollBar.vertical: Controls.ScrollBar {
            id: scrollBar
        }
        ColumnLayout {
            id: column
            width: flick.width - scrollBar.implicitWidth - Kirigami.Units.smallSpacing
            spacing: Kirigami.Units.largeSpacing
            Controls.Label {
                objectName: "playbackInfoWaiting"
                visible: overlay.sections.length === 0
                Layout.fillWidth: true
                Layout.rightMargin: closeButton.width
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                text: i18nc("@info:status", "Waiting for playback")
            }
            Repeater {
                model: overlay.sections
                delegate: ColumnLayout {
                    id: section
                    required property var modelData
                    required property int index
                    objectName: "playbackInfoSection_" + modelData.key
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.smallSpacing
                    Kirigami.Heading {
                        objectName: "playbackInfoSectionTitle_" + section.modelData.key
                        Layout.fillWidth: true
                        // The close button sits over the end of the first title
                        Layout.rightMargin: section.index === 0 ? closeButton.width : 0
                        level: 3
                        textFormat: Text.PlainText
                        text: section.modelData.title
                    }
                    Repeater {
                        model: section.modelData.rows
                        delegate: RowLayout {
                            id: row
                            required property var modelData
                            // The live row shows once its text is known
                            visible: !modelData.live || overlay.sizesText.length > 0
                            Layout.fillWidth: true
                            spacing: Kirigami.Units.largeSpacing
                            Controls.Label {
                                Layout.preferredWidth: Kirigami.Units.gridUnit * 9
                                Layout.alignment: Qt.AlignTop
                                wrapMode: Text.Wrap
                                color: Kirigami.Theme.disabledTextColor
                                textFormat: Text.PlainText
                                text: row.modelData.label
                            }
                            Controls.Label {
                                objectName: "playbackInfoValue_" + row.modelData.key
                                Layout.fillWidth: true
                                Layout.alignment: Qt.AlignTop
                                wrapMode: Text.Wrap
                                textFormat: Text.PlainText
                                text: row.modelData.live ? overlay.sizesText : row.modelData.text
                            }
                        }
                    }
                }
            }
        }
    }
    Controls.ToolButton {
        id: closeButton
        objectName: "playbackInfoCloseButton"
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: Kirigami.Units.smallSpacing
        hoverEnabled: true
        focusPolicy: Qt.TabFocus
        display: Controls.AbstractButton.IconOnly
        icon.name: "window-close"
        text: i18nc("@action:button", "Close Playback Info")
        Controls.ToolTip.text: text
        Controls.ToolTip.visible: hovered || visualFocus
        Controls.ToolTip.delay: Kirigami.Units.toolTipDelay
        onClicked: overlay.closeRequested()
    }
}
