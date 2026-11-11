// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.formcard as FormCard

FormCard.FormCardPage {
    id: page
    objectName: "settingsPage"

    required property ClientSettings settings

    title: i18n("Settings")

    readonly property var sections: [
        { section: "main", title: i18n("General"), icon: "preferences-system-windows" },
        { section: "audio", title: i18n("Audio"), icon: "audio-speakers" },
        { section: "video", title: i18n("Video"), icon: "video-display" },
        { section: "subtitles", title: i18n("Subtitles"), icon: "media-view-subtitles" },
        { section: "library", title: i18n("Library"), icon: "folder-videos" },
        { section: "other", title: i18n("mpv"), icon: "preferences-other" }
    ]

    FormCard.FormCard {
        Layout.topMargin: Kirigami.Units.gridUnit

        Repeater {
            model: page.sections
            delegate: FormCard.FormButtonDelegate {
                required property var modelData
                text: modelData.title
                icon.name: modelData.icon
                onClicked: page.Window.window.pageStack.push(Qt.resolvedUrl("SettingsSectionPage.qml"), {
                    settings: page.settings,
                    section: modelData.section,
                    title: modelData.title
                })
            }
        }
    }
}
