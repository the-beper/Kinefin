// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.delegates as Delegates
import org.kde.kirigamiaddons.formcard as FormCard

// The settings of this device and, while signed in, the preferences the user keeps on the Jellyfin server: a list of entries beside one content pane. The client settings apply at once; the User entry is a draft with its own Save bar. In a narrow window the list shows alone and an entry opens in place of it (Back closes the entry before it leaves Settings)
Kirigami.Page {
    id: page
    objectName: "settingsPage"
    required property ClientSettings settings
    required property ConnectionController connection

    // The row toolbar is ToolBar everywhere (Main.qml): this page keeps its identity in the top bar instead
    globalToolBarStyle: Kirigami.ApplicationHeaderStyle.None

    title: i18n("Settings")
    padding: 0

    readonly property bool signedIn: connection.state === ConnectionController.Authenticated
    // The height of the bar at the bottom of the page: the mini player docks above it instead of over its buttons
    readonly property real miniPlayerClearance: saveBar.visible ? saveBar.height : 0
    readonly property var deviceEntries: [
        { section: "main", title: i18n("General"), icon: "preferences-system-windows" },
        { section: "audio", title: i18n("Audio"), icon: "audio-speakers" },
        { section: "video", title: i18n("Video"), icon: "video-display" },
        { section: "subtitles", title: i18n("Subtitles"), icon: "media-view-subtitles-symbolic" },
        { section: "library", title: i18n("Library"), icon: "folder-videos" },
        { section: "other", title: i18n("mpv Configuration"), icon: "preferences-other" }
    ]
    readonly property var accountEntries: signedIn ? [{ section: "user", title: i18n("User"), icon: "preferences-desktop-user" }] : []
    readonly property var entries: deviceEntries.concat(accountEntries)
    property string activeSection: "main"
    // The User form holds dozens of rows and takes a moment to build: it starts a few frames after the entry is chosen, so the list, the heading and the spinner show the choice first (built asynchronously instead, it crashed in the tests, cause not found)
    property bool userFormReady: false
    onActiveSectionChanged: {
        userFormReady = false;
        if (activeSection === "user")
            userFormDelay.restart();
    }
    Component.onCompleted: {
        if (activeSection === "user")
            userFormDelay.restart();
    }
    Timer {
        id: userFormDelay
        interval: 50
        onTriggered: page.userFormReady = page.activeSection === "user"
    }
    // Narrow windows only: the entry shows in place of the list
    property bool entryOpen: false
    // The list folds where the shell's wide mode ends (Main.qml owns the limit)
    readonly property bool narrow: width < (page.Window.window?.wideLimit ?? 0)
    readonly property var activeEntry: entries.find(entry => entry.section === activeSection) ?? entries[0]
    readonly property real contentMargin: Kirigami.Units.largeSpacing * 2
    // The Flickable the scroll view makes of its content
    readonly property var scroller: settingsView.contentItem
    // The form of the User entry while it is alive: one form at a time, whatever the width (the Loader is the same in both modes)
    readonly property var userFormItem: userForm.item
    // The shell reads this before it drops the page
    readonly property bool hasUnsavedChanges: userFormItem?.hasUnsavedChanges ?? false
    // The entry of the other list that holds the same kind of choice: the preferences on the server and the settings of this device overlap on audio, subtitles and libraries
    readonly property string twinText: {
        switch (activeSection) {
        case "audio":
            return i18n("Audio language and tracks");
        case "subtitles":
            return i18n("Subtitle language and mode");
        case "library":
            return i18n("Libraries on Home and their order");
        }
        return "";
    }

    // Runs `proceed` at once, or once the user saved or discarded the draft of the User entry
    function askBeforeLeaving(proceed) {
        if (hasUnsavedChanges)
            userFormItem.askBeforeLeaving(proceed);
        else
            proceed();
    }

    // Leaving the User entry for another asks first; the entry already showing is not opened again
    function selectSection(section) {
        if (section === activeSection && entryOpen)
            return;
        const open = () => {
            activeSection = section;
            entryOpen = true;
            if (scroller)
                scroller.contentY = 0;
        };
        if (activeSection === "user" && section !== "user")
            askBeforeLeaving(open);
        else
            open();
    }

    function focusPrimary() {
        if (narrow && entryOpen) {
            if (activeSection === "user" && userFormItem)
                userFormItem.focusPrimary();
            else
                settingsView.forceActiveFocus(Qt.TabFocusReason);
            return;
        }
        entryRepeater.itemAt(entries.findIndex(entry => entry.section === activeSection))?.forceActiveFocus(Qt.TabFocusReason);
    }

    // Tab through a long form scrolls the view to the row that takes the keyboard
    function revealFocus() {
        const flickable = scroller;
        const target = page.Window.window?.activeFocusItem;
        if (!target || !flickable?.contentItem)
            return;
        let inside = false;
        for (let item = target; item && !inside; item = item.parent)
            inside = item === flickable.contentItem;
        if (!inside)
            return;
        const top = target.mapToItem(flickable.contentItem, 0, 0).y;
        const bottom = top + target.height;
        const margin = Kirigami.Units.largeSpacing;
        const last = Math.max(0, flickable.contentHeight - flickable.height);
        if (top < flickable.contentY + margin)
            flickable.contentY = Math.max(0, top - margin);
        else if (bottom > flickable.contentY + flickable.height - margin)
            flickable.contentY = Math.min(last, bottom - flickable.height + margin);
    }

    onBackRequested: event => {
        if (narrow && entryOpen) {
            // The entry closes first, and with a draft it asks
            event.accepted = true;
            askBeforeLeaving(() => page.entryOpen = false);
            return;
        }
        if (!hasUnsavedChanges)
            return;
        event.accepted = true;
        askBeforeLeaving(() => page.Window.window.pageStack.goBack());
    }
    onEntryOpenChanged: {
        if (narrow)
            Qt.callLater(focusPrimary);
    }
    // The session ended under the User entry
    onSignedInChanged: {
        if (!signedIn && activeSection === "user") {
            activeSection = "main";
            entryOpen = false;
        }
    }

    Connections {
        target: page.Window.window
        function onActiveFocusItemChanged() {
            page.revealFocus();
        }
    }

    Component {
        id: entryDelegate

        Delegates.RoundedItemDelegate {
            id: entry

            required property var modelData

            objectName: "settingsCategory_" + modelData.section
            Layout.fillWidth: true
            text: modelData.title
            icon.name: modelData.icon
            checked: !page.narrow && page.activeSection === modelData.section
            onClicked: {
                // The keys follow the pointer: the row the page focused when it opened would keep its focus frame and look chosen too
                entry.forceActiveFocus(Qt.MouseFocusReason);
                page.selectSection(modelData.section);
            }
            Accessible.name: modelData.title
        }
    }

    Component {
        id: userFormComponent

        UserPreferencesForm {
            connection: page.connection
            onLinkRequested: section => page.selectSection(section)
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Item {
            id: listPane
            objectName: "settingsEntryList"
            Layout.fillHeight: true
            Layout.fillWidth: page.narrow
            Layout.preferredWidth: Kirigami.Units.gridUnit * 13
            visible: !page.narrow || !page.entryOpen

            Kirigami.Theme.colorSet: Kirigami.Theme.View
            Kirigami.Theme.inherit: false

            Rectangle {
                anchors.fill: parent
                color: Kirigami.Theme.backgroundColor
            }

            Controls.ScrollView {
                anchors.fill: parent
                contentWidth: availableWidth

                ColumnLayout {
                    width: Math.max(0, parent.width)
                    spacing: 0

                    // The page has no title bar: the list names itself, as System Settings does
                    Kirigami.ListSectionHeader {
                        objectName: "settingsListHeader"
                        Layout.fillWidth: true
                        // A heading, never a tab stop
                        focusPolicy: Qt.NoFocus
                        text: i18nc("@title:group", "Settings")
                    }

                    Repeater {
                        id: entryRepeater
                        model: page.entries
                        delegate: entryDelegate
                    }
                }
            }
        }
        Kirigami.Separator {
            Layout.fillHeight: true
            visible: !page.narrow
        }
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !page.narrow || page.entryOpen
            spacing: 0

            Controls.ScrollView {
                id: settingsView
                objectName: "settingsContentView"
                Layout.fillWidth: true
                Layout.fillHeight: true
                contentWidth: availableWidth

                ColumnLayout {
                    // The room of the vertical scroll bar stays free while the bar hides, so the centred content keeps its place when the bar comes and goes (the bar takes the left side in a right-to-left layout)
                    readonly property var scrollBar: settingsView.Controls.ScrollBar.vertical
                    readonly property real scrollBarRoom: scrollBar && scrollBar.interactive && !scrollBar.visible ? scrollBar.width : 0
                    x: settingsView.mirrored ? scrollBarRoom : 0
                    width: settingsView.availableWidth - scrollBarRoom
                    spacing: Kirigami.Units.largeSpacing

                    Kirigami.Heading {
                        objectName: "settingsContentHeading"
                        Layout.fillWidth: true
                        Layout.maximumWidth: Kirigami.Units.gridUnit * 30
                        Layout.alignment: Qt.AlignHCenter
                        Layout.topMargin: Kirigami.Units.largeSpacing * 2
                        Layout.leftMargin: page.contentMargin
                        Layout.rightMargin: page.contentMargin
                        level: 1
                        textFormat: Text.PlainText
                        text: page.activeEntry.title
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.leftMargin: page.contentMargin
                        Layout.rightMargin: page.contentMargin
                        spacing: 0
                        visible: page.activeSection !== "user"

                        Repeater {
                            model: page.settings.sectionGroups(page.activeSection)
                            delegate: ColumnLayout {
                                id: group

                                required property string modelData
                                required property int index

                                Layout.fillWidth: true
                                spacing: 0

                                FormCard.FormHeader {
                                    visible: group.modelData.length > 0
                                    title: group.modelData
                                }
                                FormCard.FormCard {
                                    objectName: "settingsGroupCard_" + group.index
                                    Layout.fillWidth: true

                                    Repeater {
                                        model: page.settings.sectionModel(page.activeSection, group.modelData)
                                        delegate: SettingRow {
                                            groupTitle: group.modelData
                                        }
                                    }
                                }
                            }
                        }
                        FormCard.FormHeader {
                            visible: twinCard.visible
                            title: i18nc("@title:group", "Jellyfin account")
                        }
                        FormCard.FormCard {
                            id: twinCard
                            objectName: "settingsTwinCard"
                            Layout.fillWidth: true
                            visible: page.signedIn && page.twinText.length > 0

                            FormCard.FormButtonDelegate {
                                objectName: "settingsLinkToUser_" + page.activeSection
                                text: page.twinText
                                description: i18n("Stored on your Jellyfin server")
                                onClicked: page.selectSection("user")
                            }
                        }
                    }
                    Loader {
                        id: userForm
                        objectName: "userPreferencesLoader"
                        Layout.fillWidth: true
                        Layout.leftMargin: page.contentMargin
                        Layout.rightMargin: page.contentMargin
                        active: page.signedIn && page.activeSection === "user" && page.userFormReady
                        visible: active
                        sourceComponent: userFormComponent
                        // A narrow window opened the entry while the form was still coming: it takes the keys now
                        onLoaded: {
                            if (page.narrow && page.entryOpen && settingsView.activeFocus)
                                page.focusPrimary();
                        }
                    }
                    Kirigami.LoadingPlaceholder {
                        objectName: "userPreferencesBuilding"
                        Layout.alignment: Qt.AlignHCenter
                        Layout.topMargin: Kirigami.Units.gridUnit * 2
                        visible: page.signedIn && page.activeSection === "user" && userForm.status !== Loader.Ready
                        text: i18n("Loading…")
                    }
                    Item {
                        Layout.preferredHeight: Kirigami.Units.largeSpacing
                    }
                }
            }

            UserPreferencesSaveBar {
                id: saveBar
                objectName: "settingsSaveBar"
                Layout.fillWidth: true
                prefs: page.connection.userConfiguration
                visible: page.activeSection === "user" && page.userFormItem !== null && page.connection.userConfiguration.loaded
            }
        }
    }
}
