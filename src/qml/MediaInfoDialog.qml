// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The container, size, bitrate and streams of the media sources of an item, with a Copy action for the whole text
Kirigami.Dialog {
    id: dialog

    // The mediaSources of the item details: server data, shown as plain text only
    property var sources: []
    // The window that shows the passive notification
    property var window: null
    readonly property var entries: sources.map(source => ({ name: source.name, rows: sourceRows(source) }))

    title: i18nc("@title:window", "Media Info")
    preferredWidth: Kirigami.Units.gridUnit * 30
    maximumWidth: Kirigami.Units.gridUnit * 34
    maximumHeight: absoluteMaximumHeight - Kirigami.Units.gridUnit * 4
    customFooterActions: [
        Kirigami.Action {
            objectName: "mediaInfoCopyAction"
            text: i18nc("@action:button", "Copy")
            icon.name: "edit-copy"
            onTriggered: dialog.copyInfo()
        }
    ]

    // One line per stream: codec, resolution or channels, language, title and flags
    function streamText(stream, kind) {
        const parts = [];
        if (stream.codec.length > 0)
            parts.push(stream.codec.toUpperCase());
        if (kind === "video" && stream.width > 0 && stream.height > 0)
            parts.push(stream.width + "×" + stream.height);
        if (kind === "audio" && stream.channels > 0)
            parts.push(i18np("%1 channel", "%1 channels", stream.channels));
        if (stream.language.length > 0)
            parts.push(stream.language);
        if (stream.title.length > 0)
            parts.push(stream.title);
        if (stream.isDefault)
            parts.push(i18n("Default"));
        if (stream.isForced)
            parts.push(i18n("Forced"));
        if (stream.isExternal)
            parts.push(i18n("External"));
        return parts.join(" · ");
    }

    // The rows of one source: label and value lines
    function sourceRows(source) {
        const rows = [];
        if (source.container.length > 0)
            rows.push({ label: i18n("Container"), text: source.container });
        if (source.size > 0)
            rows.push({ label: i18n("Size"), text: Qt.locale().formattedDataSize(source.size, 1) });
        if (source.bitrate > 0)
            rows.push({ label: i18n("Bitrate"), text: i18n("%1 Mbps", (source.bitrate / 1000000).toLocaleString(Qt.locale(), "f", 1)) });
        const streams = [
            { label: i18n("Video"), kind: "video", list: source.video },
            { label: i18n("Audio"), kind: "audio", list: source.audio },
            { label: i18n("Subtitles"), kind: "subtitles", list: source.subtitles }
        ];
        for (const entry of streams) {
            if (entry.list.length > 0)
                rows.push({ label: entry.label, text: entry.list.map(stream => streamText(stream, entry.kind)).join("\n") });
        }
        return rows;
    }

    // The source name, then one "Label: value" line per row with the further lines of a value indented; sources are separated by a blank line
    function infoText() {
        return entries.map(entry => {
            const lines = entry.name.length > 0 ? [entry.name] : [];
            for (const row of entry.rows)
                lines.push(row.label + ": " + row.text.split("\n").join("\n    "));
            return lines.join("\n");
        }).join("\n\n");
    }

    function copyInfo() {
        clipboard.text = infoText();
        clipboard.selectAll();
        clipboard.copy();
        window?.showPassiveNotification(i18n("Media info copied to the clipboard"));
    }

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        // Qt Quick has no clipboard API: a hidden TextEdit selects its text and copies it
        TextEdit {
            id: clipboard
            visible: false
            textFormat: TextEdit.PlainText
        }

        Repeater {
            model: dialog.entries
            delegate: ColumnLayout {
                id: source

                required property var modelData

                Layout.fillWidth: true
                spacing: Kirigami.Units.smallSpacing

                QQC2.Label {
                    Layout.fillWidth: true
                    visible: text.length > 0
                    wrapMode: Text.Wrap
                    font.bold: true
                    textFormat: Text.PlainText
                    text: source.modelData.name
                }

                GridLayout {
                    Layout.fillWidth: true
                    columns: 2
                    columnSpacing: Kirigami.Units.largeSpacing
                    rowSpacing: Kirigami.Units.smallSpacing

                    // Two cells per row: the label, then the value
                    Repeater {
                        model: source.modelData.rows.length * 2
                        delegate: QQC2.Label {
                            id: cell

                            required property int index
                            readonly property bool isValue: index % 2 === 1
                            readonly property var row: source.modelData.rows[Math.floor(index / 2)]

                            Layout.alignment: Qt.AlignTop
                            Layout.fillWidth: isValue
                            opacity: isValue ? 1 : 0.7
                            wrapMode: Text.Wrap
                            textFormat: Text.PlainText
                            text: isValue ? row?.text ?? "" : row?.label ?? ""
                        }
                    }
                }
            }
        }
    }
}
