// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.formcard as FormCard

// System Settings-style split view. In a narrow window the category list is
// shown first and a category opens as its own page (with the normal Back key).
Kirigami.Page {
    id: page
    objectName: "settingsPage"
    required property ClientSettings settings

    // The row toolbar is ToolBar everywhere (Main.qml): this page keeps its identity in the top bar instead
    globalToolBarStyle: Kirigami.ApplicationHeaderStyle.None

    title: i18n("Settings")
    padding: 0

    readonly property var sections: [
        { section: "main", title: i18n("General"), icon: "preferences-system-windows" },
        { section: "audio", title: i18n("Audio"), icon: "audio-speakers" },
        { section: "video", title: i18n("Video"), icon: "video-display" },
        { section: "subtitles", title: i18n("Subtitles"), icon: "media-view-subtitles-symbolic" },
        { section: "library", title: i18n("Library"), icon: "folder-videos" },
        { section: "other", title: i18n("mpv"), icon: "preferences-other" }
    ]
    property string activeSection: "main"
    // The category list folds where the shell's wide mode ends (Main.qml owns the limit)
    readonly property bool narrow: width < (page.Window.window?.wideLimit ?? 0)
    readonly property var activeEntry: sections.find(entry => entry.section === activeSection) ?? sections[0]

    function selectSection(section) {
        if (narrow) {
            const entry = sections.find(candidate => candidate.section === section);
            if (entry)
                page.Window.window.pageStack.push(Qt.resolvedUrl("SettingsSectionPage.qml"), {
                    settings: page.settings, section: entry.section, title: entry.title
                });
        } else {
            activeSection = section;
            if (settingsView.contentItem)
                settingsView.contentItem.contentY = 0;
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.preferredWidth: Math.min(Kirigami.Units.gridUnit * 16, page.width * 0.36)
            Layout.fillHeight: true
            color: Kirigami.Theme.alternateBackgroundColor
            visible: !page.narrow

            Controls.ScrollView {
                anchors.fill: parent
                contentWidth: availableWidth
                ColumnLayout {
                    width: Math.max(0, parent.width)
                    spacing: Kirigami.Units.smallSpacing
                    Kirigami.Heading {
                        Layout.fillWidth: true
                        Layout.margins: Kirigami.Units.largeSpacing
                        level: 2
                        text: i18n("Settings")
                    }
                    Repeater {
                        model: page.sections
                        delegate: Controls.ItemDelegate {
                            id: entry
                            required property var modelData
                            objectName: "settingsCategory_" + modelData.section
                            Layout.fillWidth: true
                            Layout.leftMargin: Kirigami.Units.smallSpacing
                            Layout.rightMargin: Kirigami.Units.smallSpacing
                            text: modelData.title
                            icon.name: modelData.icon
                            highlighted: page.activeSection === modelData.section
                            onClicked: page.selectSection(modelData.section)
                            Accessible.name: modelData.title
                        }
                    }
                }
            }
        }
        Kirigami.Separator {
            Layout.fillHeight: true
            visible: !page.narrow
        }
        Controls.ScrollView {
            id: settingsView
            objectName: "settingsContentView"
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !page.narrow
            contentWidth: availableWidth

            ColumnLayout {
                width: settingsView.availableWidth
                spacing: Kirigami.Units.largeSpacing
                Kirigami.Heading {
                    objectName: "settingsContentHeading"
                    Layout.fillWidth: true
                    Layout.maximumWidth: contentCard.maximumWidth
                    Layout.alignment: Qt.AlignHCenter
                    Layout.topMargin: Kirigami.Units.largeSpacing * 2
                    Layout.leftMargin: Kirigami.Units.largeSpacing * 2
                    Layout.rightMargin: Kirigami.Units.largeSpacing * 2
                    level: 1
                    text: page.activeEntry.title
                }
                FormCard.FormCard {
                    id: contentCard
                    objectName: "settingsContentCard"
                    Layout.fillWidth: true
                    Layout.leftMargin: Kirigami.Units.largeSpacing * 2
                    Layout.rightMargin: Kirigami.Units.largeSpacing * 2
                    Repeater {
                        model: page.settings.sectionModel(page.activeSection)
                        delegate: SettingRow {}
                    }
                }
                Item { Layout.preferredHeight: Kirigami.Units.largeSpacing }
            }
        }
        // Narrow windows use a single column; no horizontally clipped forms.
        Controls.ScrollView {
            id: narrowSettingsView
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: page.narrow
            contentWidth: availableWidth
            ColumnLayout {
                width: narrowSettingsView.availableWidth
                spacing: Kirigami.Units.smallSpacing
                Kirigami.Heading {
                    Layout.fillWidth: true
                    Layout.maximumWidth: categoryCard.maximumWidth
                    Layout.alignment: Qt.AlignHCenter
                    Layout.margins: Kirigami.Units.largeSpacing
                    level: 2
                    text: i18n("Settings")
                }
                FormCard.FormCard {
                    id: categoryCard
                    Layout.fillWidth: true
                    Layout.margins: Kirigami.Units.largeSpacing
                    Repeater {
                        model: page.sections
                        delegate: FormCard.FormButtonDelegate {
                            required property var modelData
                            objectName: "settingsCompactCategory_" + modelData.section
                            text: modelData.title
                            icon.name: modelData.icon
                            onClicked: page.selectSection(modelData.section)
                        }
                    }
                }
            }
        }
    }
}
