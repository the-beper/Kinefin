// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.kde.kirigami as Kirigami

// The page of a section that is not there yet (Music, Audiobooks): a root tab like the others, with a message and nothing else; it asks the server for nothing
Kirigami.Page {
    id: page
    objectName: "placeholderSectionPage"

    // "music" or "audiobooks"
    required property string section

    // No title bar and no toolbar: the rail marks the section and the message names it
    globalToolBarStyle: Kirigami.ApplicationHeaderStyle.None
    // Without Kirigami's title row the page names itself to a screen reader
    Accessible.role: Accessible.Pane
    Accessible.name: message.text

    Kirigami.PlaceholderMessage {
        id: message
        objectName: "placeholderSectionMessage"
        anchors.centerIn: parent
        width: parent.width - Kirigami.Units.gridUnit * 4
        icon.name: page.section === "music" ? "folder-music-symbolic" : "audio-headphones-symbolic"
        text: page.section === "music" ? i18n("Music is coming soon") : i18n("Audiobooks are coming soon")
    }
}
