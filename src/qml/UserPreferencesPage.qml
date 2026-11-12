// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.formcard as FormCard

// The preferences the signed-in user keeps on the Jellyfin server (UserConfiguration). The model holds the draft and the whole read-modify-write cycle; this page only edits it, and leaving with a draft asks first (the shell calls askBeforeLeaving before it drops the page)
FormCard.FormCardPage {
    id: page
    objectName: "userPreferencesPage"

    required property ConnectionController connection

    readonly property UserConfigurationModel prefs: connection.userConfiguration
    // The shell reads this before it drops the page
    readonly property bool hasUnsavedChanges: prefs.dirty
    readonly property bool saving: prefs.state === UserConfigurationModel.Saving || prefs.state === UserConfigurationModel.Verifying
    readonly property bool busy: saving || prefs.state === UserConfigurationModel.Loading
    // What to do once a save started from the unsaved changes dialog is confirmed
    property var pendingLeave: null
    // True while the form could not be edited: the keyboard goes back to it when it can
    property bool wasBlocked: true

    title: i18n("User Preferences")

    function focusPrimary() {
        if (prefs.editable)
            audioLanguage.focusBox();
        else
            page.forceActiveFocus();
    }

    function isMalformed(key) {
        return prefs.malformedFields.indexOf(key) >= 0;
    }

    function malformedNote() {
        return i18n("The server stores this in a form Kinefin cannot edit");
    }

    function libraryName(id) {
        const wanted = id.toLowerCase();
        const library = prefs.libraries.find(entry => entry.id.toLowerCase() === wanted);
        return library ? library.name : "";
    }

    function listsId(ids, id) {
        const wanted = id.toLowerCase();
        return ids.some(entry => entry.toLowerCase() === wanted);
    }

    // The modes of the server's enum; one this client does not know stays selected as such until another is chosen
    readonly property var subtitleModeEntries: {
        const labels = {
            "Default": i18nc("@item:inlistbox subtitle mode", "Default"),
            "Always": i18nc("@item:inlistbox subtitle mode", "Always show subtitles"),
            "OnlyForced": i18nc("@item:inlistbox subtitle mode", "Only forced subtitles"),
            "None": i18nc("@item:inlistbox subtitle mode", "No subtitles"),
            "Smart": i18nc("@item:inlistbox subtitle mode", "Smart")
        };
        const entries = prefs.subtitleModes.map(mode => ({ value: mode, text: labels[mode] ?? mode }));
        if (prefs.subtitleMode.length > 0 && prefs.subtitleModes.indexOf(prefs.subtitleMode) < 0)
            entries.push({ value: prefs.subtitleMode, text: i18nc("@item:inlistbox subtitle mode", "Not supported by Kinefin") });
        return entries;
    }

    // The shell asks before it drops this page (a section or a tab opens, Ctrl+F, closing the window): `proceed` runs at once when no draft is at stake, else once the user chose to save or discard it. Cancel, and a save that does not end well, drop it
    function askBeforeLeaving(proceed) {
        if (!prefs.dirty) {
            proceed();
        } else if (saving) {
            pendingLeave = proceed;
        } else {
            leaveDialog.proceed = proceed;
            leaveDialog.open();
        }
    }

    // The row moves one place and the keyboard follows it: the rows are rebuilt with the new order
    function moveLibrary(from, delta) {
        const to = from + delta;
        if (to < 0 || to >= prefs.orderedViews.length)
            return;
        prefs.moveOrderedView(from, to);
        Qt.callLater(() => orderRepeater.itemAt(to)?.forceActiveFocus(Qt.TabFocusReason));
    }

    onBackRequested: event => {
        if (!prefs.dirty)
            return;
        event.accepted = true;
        askBeforeLeaving(() => page.Window.window.pageStack.goBack());
    }

    Component.onCompleted: prefs.load()
    // The draft is not kept: the next visit reads the server again
    Component.onDestruction: {
        const state = prefs?.state;
        if (prefs?.dirty && (state === UserConfigurationModel.Ready || state === UserConfigurationModel.Conflict || (state === UserConfigurationModel.Failed && prefs.failure === UserConfigurationModel.SaveFailed)))
            prefs.discardDraft();
    }

    Connections {
        target: page.prefs
        function onSaved() {
            page.Window.window?.showPassiveNotification(i18n("Preferences saved"));
            const leave = page.pendingLeave;
            page.pendingLeave = null;
            if (leave)
                leave();
        }
        function onStateChanged() {
            const state = page.prefs.state;
            if (state === UserConfigurationModel.Conflict || state === UserConfigurationModel.Failed)
                page.pendingLeave = null;
            const blocked = !page.prefs.editable;
            if (page.wasBlocked && !blocked)
                Qt.callLater(() => page.Window.window?.focusCurrentPage());
            page.wasBlocked = blocked;
        }
    }

    // A picker over the language catalog of the server. The stored code stays selected even when the catalog lacks it, until another is chosen. Names are server text: plain text only
    component LanguageRow: FormCard.AbstractFormDelegate {
        id: row

        required property CultureOptionsModel cultures
        // The code of the draft, empty for Automatic
        property string code
        signal picked(string selected)

        readonly property var entries: {
            let list = [{ code: "", name: i18nc("@item:inlistbox no language preference", "Automatic") }];
            if (cultures.available)
                list = list.concat(cultures.entries());
            if (code.length > 0 && !cultures.contains(code))
                list.push({ code: code, name: i18nc("@item:inlistbox %1 is a language code the server stores", "%1 (not in the catalog)", code) });
            return list;
        }
        readonly property int currentRow: {
            if (code.length === 0)
                return 0;
            const index = cultures.indexOfCode(code);
            return index >= 0 ? index + 1 : entries.length - 1;
        }

        function focusBox() {
            box.forceActiveFocus(Qt.TabFocusReason);
        }

        // The box is the tab stop, a click on the rest of the row opens it
        focusPolicy: Qt.NoFocus
        onClicked: box.popup.open()

        contentItem: GridLayout {
            id: grid

            columns: row.width > Kirigami.Units.gridUnit * 28 ? 2 : 1
            columnSpacing: Kirigami.Units.largeSpacing
            rowSpacing: Kirigami.Units.smallSpacing

            Controls.Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                text: row.text
                Accessible.ignored: true
            }

            Controls.ComboBox {
                id: box

                Layout.fillWidth: grid.columns === 1
                Layout.preferredWidth: Kirigami.Units.gridUnit * 14
                model: row.entries
                textRole: "name"
                currentIndex: row.currentRow
                Accessible.name: row.text
                onActivated: index => row.picked(row.entries[index].code)

                // A new list puts the box back on its first entry without changing the value the binding computes (the code keeps the same position when the catalog arrives), so the choice is bound again
                function followChoice() {
                    currentIndex = Qt.binding(() => row.currentRow);
                }
                onModelChanged: {
                    followChoice();
                    Qt.callLater(followChoice);
                }
                onCountChanged: {
                    followChoice();
                    Qt.callLater(followChoice);
                }

                // The style paints the selected name itself, as plain text; a custom contentItem breaks it, which reads text input API
                delegate: Controls.ItemDelegate {
                    id: entry

                    required property var modelData
                    required property int index

                    width: ListView.view ? ListView.view.width : implicitWidth
                    highlighted: box.highlightedIndex === entry.index
                    contentItem: Controls.Label {
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        text: entry.modelData.name
                    }
                }
            }

            Controls.Label {
                Layout.fillWidth: true
                Layout.columnSpan: grid.columns
                visible: !row.cultures.available
                wrapMode: Text.Wrap
                opacity: 0.7
                textFormat: Text.PlainText
                text: i18n("The language list is not available: only Automatic and the current choice can be selected")
            }
        }
    }

    // A check box that follows a value of the draft (nothing applies until Save, so no switch). The stock delegate writes its own `checked` back while it follows its inner check box, which drops a plain binding on it with the first change: the value source is a Binding, which keeps working
    component PreferenceCheck: FormCard.FormCheckDelegate {
        id: preferenceCheck

        property bool draftValue

        Binding on checked {
            value: preferenceCheck.draftValue
        }
    }

    // The background of a row that is created and deleted with its list. The stock one reads the parent of its row, which is already gone while the row is being deleted
    component RowBackground: Rectangle {
        // The row this paints: a template button, which is not the Controls type, so it is read without a type
        required property var control

        color: {
            const text = Kirigami.Theme.textColor;
            const opacity = !control?.enabled ? 0 : control.pressed ? 0.2 : control.visualFocus ? 0.1 : control.hovered ? 0.07 : 0;
            return Qt.rgba(text.r, text.g, text.b, opacity);
        }
    }

    // A library with a check box. The name is server text and goes in a plain-text label: the stock form check delegate would render it as rich text
    component LibraryCheckRow: FormCard.AbstractFormDelegate {
        id: row

        // What a screen reader says: the row text alone does not tell what checking it does
        property string accessibleLabel

        checkable: true
        background: RowBackground {
            control: row
        }
        Accessible.role: Accessible.CheckBox
        Accessible.name: accessibleLabel.length > 0 ? accessibleLabel : text
        Accessible.checked: checked

        contentItem: RowLayout {
            spacing: Kirigami.Units.largeSpacing

            Controls.CheckBox {
                focusPolicy: Qt.NoFocus
                checked: row.checked
                contentItem: null
                spacing: 0
                padding: 0
                // The row is the source of truth: use clicked, not toggled, because syncing `checked` from the row must never toggle it again; row.toggle() emits toggled itself, updating the draft once
                onClicked: row.toggle()
                Accessible.ignored: true
            }

            Controls.Label {
                Layout.fillWidth: true
                elide: Text.ElideRight
                textFormat: Text.PlainText
                text: row.text
            }
        }
    }

    // A message of the page, as wide as the cards
    component PageMessage: MessageBox {
        Layout.maximumWidth: Kirigami.Units.gridUnit * 30
        Layout.alignment: Qt.AlignHCenter
        Layout.margins: Kirigami.Units.largeSpacing
    }

    Item {
        Layout.fillWidth: true
        Layout.preferredHeight: Kirigami.Units.gridUnit * 10
        visible: !page.prefs.loaded && page.prefs.state !== UserConfigurationModel.Failed

        Kirigami.LoadingPlaceholder {
            anchors.centerIn: parent
            text: i18n("Loading…")
        }
    }

    PageMessage {
        objectName: "prefsNote"
        visible: page.prefs.loaded
        type: Kirigami.MessageType.Information
        text: i18n("These preferences are stored on your Jellyfin server. They do not change the language of Kinefin, and the player will apply the language and subtitle choices in a later version")
    }

    PageMessage {
        objectName: "prefsError"
        visible: page.prefs.state === UserConfigurationModel.Failed
        type: page.prefs.failure === UserConfigurationModel.NotVerified ? Kirigami.MessageType.Warning : Kirigami.MessageType.Error
        text: {
            switch (page.prefs.failure) {
            case UserConfigurationModel.InvalidProfile:
                return i18n("The server sent preferences that Kinefin cannot read");
            case UserConfigurationModel.SaveFailed:
                return i18n("Could not save the preferences, your changes are kept: check the connection to the server and try again");
            case UserConfigurationModel.NotVerified:
                return i18n("The preferences were sent but could not be confirmed: reload them before trusting what is shown");
            }
            return i18n("Could not load the preferences: check the connection to the server and try again");
        }
        actions: [
            Kirigami.Action {
                text: page.prefs.failure === UserConfigurationModel.NotVerified ? i18nc("@action:button", "Reload") : i18nc("@action:button", "Retry")
                icon.name: "view-refresh"
                onTriggered: page.prefs.retry()
            },
            Kirigami.Action {
                text: i18nc("@action:button", "Keep Editing")
                icon.name: "document-edit"
                visible: page.prefs.failure === UserConfigurationModel.SaveFailed && page.prefs.loaded
                onTriggered: page.prefs.keepEditing()
            }
        ]
    }

    PageMessage {
        objectName: "prefsConflict"
        visible: page.prefs.state === UserConfigurationModel.Conflict
        type: Kirigami.MessageType.Warning
        text: i18n("Some preferences were changed on another system, so nothing was saved: keep editing to review your changes, or discard them")
        actions: [
            Kirigami.Action {
                text: i18nc("@action:button", "Keep Editing")
                icon.name: "document-edit"
                onTriggered: page.prefs.keepEditing()
            },
            Kirigami.Action {
                text: i18nc("@action:button", "Discard")
                icon.name: "edit-undo"
                onTriggered: page.prefs.discard()
            }
        ]
    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: 0
        visible: page.prefs.loaded
        enabled: page.prefs.editable

        FormCard.FormHeader {
            title: i18nc("@title:group", "Audio")
        }
        FormCard.FormCard {
            LanguageRow {
                id: audioLanguage
                objectName: "prefAudioLanguage"

                text: i18n("Audio language")
                enabled: !page.isMalformed("AudioLanguagePreference")
                cultures: page.prefs.cultures
                code: page.prefs.audioLanguage
                onPicked: selected => page.prefs.audioLanguage = selected
            }
            PreferenceCheck {
                objectName: "prefPlayDefaultAudioTrack"

                text: i18n("Play the default audio track")
                description: page.isMalformed("PlayDefaultAudioTrack") ? page.malformedNote() : i18n("Use the track the file marks as default, whatever its language")
                enabled: !page.isMalformed("PlayDefaultAudioTrack")
                draftValue: page.prefs.playDefaultAudioTrack
                onToggled: page.prefs.playDefaultAudioTrack = checked
            }
            PreferenceCheck {
                objectName: "prefRememberAudioSelections"

                text: i18n("Remember the audio track")
                description: page.isMalformed("RememberAudioSelections") ? page.malformedNote() : i18n("Start an item with the audio track chosen for the previous one when possible")
                enabled: !page.isMalformed("RememberAudioSelections")
                draftValue: page.prefs.rememberAudioSelections
                onToggled: page.prefs.rememberAudioSelections = checked
            }
        }

        FormCard.FormHeader {
            title: i18nc("@title:group", "Subtitles")
        }
        FormCard.FormCard {
            LanguageRow {
                objectName: "prefSubtitleLanguage"

                text: i18n("Subtitle language")
                enabled: !page.isMalformed("SubtitleLanguagePreference")
                cultures: page.prefs.cultures
                code: page.prefs.subtitleLanguage
                onPicked: selected => page.prefs.subtitleLanguage = selected
            }
            FormCard.FormComboBoxDelegate {
                objectName: "prefSubtitleMode"

                text: i18n("Subtitle mode")
                enabled: !page.isMalformed("SubtitleMode")
                model: page.subtitleModeEntries
                textRole: "text"
                valueRole: "value"
                currentIndex: Math.max(0, page.subtitleModeEntries.findIndex(entry => entry.value === page.prefs.subtitleMode))
                onActivated: page.prefs.subtitleMode = currentValue
            }
            PreferenceCheck {
                objectName: "prefRememberSubtitleSelections"

                text: i18n("Remember the subtitle track")
                description: page.isMalformed("RememberSubtitleSelections") ? page.malformedNote() : i18n("Start an item with the subtitles chosen for the previous one when possible")
                enabled: !page.isMalformed("RememberSubtitleSelections")
                draftValue: page.prefs.rememberSubtitleSelections
                onToggled: page.prefs.rememberSubtitleSelections = checked
            }
        }

        FormCard.FormHeader {
            title: i18nc("@title:group", "Playback")
        }
        FormCard.FormCard {
            PreferenceCheck {
                objectName: "prefEnableNextEpisodeAutoPlay"

                text: i18n("Play the next episode automatically")
                description: page.isMalformed("EnableNextEpisodeAutoPlay") ? page.malformedNote() : ""
                enabled: !page.isMalformed("EnableNextEpisodeAutoPlay")
                draftValue: page.prefs.enableNextEpisodeAutoPlay
                onToggled: page.prefs.enableNextEpisodeAutoPlay = checked
            }
        }

        FormCard.FormHeader {
            title: i18nc("@title:group", "Libraries and Home")
        }
        FormCard.FormCard {
            PreferenceCheck {
                objectName: "prefDisplayMissingEpisodes"

                text: i18n("Show missing episodes")
                description: page.isMalformed("DisplayMissingEpisodes") ? page.malformedNote() : ""
                enabled: !page.isMalformed("DisplayMissingEpisodes")
                draftValue: page.prefs.displayMissingEpisodes
                onToggled: page.prefs.displayMissingEpisodes = checked
            }
            PreferenceCheck {
                objectName: "prefHidePlayedInLatest"

                text: i18n("Leave played items out of Recently Added")
                description: page.isMalformed("HidePlayedInLatest") ? page.malformedNote() : ""
                enabled: !page.isMalformed("HidePlayedInLatest")
                draftValue: page.prefs.hidePlayedInLatest
                onToggled: page.prefs.hidePlayedInLatest = checked
            }
        }

        FormCard.FormHeader {
            title: i18nc("@title:group", "Order of the libraries")
        }
        FormCard.FormCard {
            FormCard.FormSectionText {
                text: page.isMalformed("OrderedViews") ? page.malformedNote() : i18n("The order of the library tiles on Home. Select a library and press Ctrl+Up or Ctrl+Down to move it")
            }
            FormCard.FormSectionText {
                visible: page.prefs.libraries.length === 0
                text: i18n("No libraries are available")
            }
            Repeater {
                id: orderRepeater

                model: page.prefs.orderedViews
                delegate: FormCard.AbstractFormDelegate {
                    id: orderRow

                    required property int index
                    required property string modelData

                    objectName: "prefOrderRow_" + index
                    text: page.libraryName(modelData)
                    background: RowBackground {
                        control: orderRow
                    }
                    enabled: !page.isMalformed("OrderedViews")
                    Accessible.role: Accessible.ListItem
                    Accessible.name: i18n("%1, %2 of %3", text, index + 1, page.prefs.orderedViews.length)
                    Keys.onPressed: event => {
                        if (event.modifiers === Qt.ControlModifier && event.key === Qt.Key_Up) {
                            page.moveLibrary(index, -1);
                            event.accepted = true;
                        } else if (event.modifiers === Qt.ControlModifier && event.key === Qt.Key_Down) {
                            page.moveLibrary(index, 1);
                            event.accepted = true;
                        }
                    }

                    contentItem: RowLayout {
                        spacing: Kirigami.Units.smallSpacing

                        Controls.Label {
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            textFormat: Text.PlainText
                            text: orderRow.text
                        }
                        Controls.ToolButton {
                            objectName: "prefMoveUp_" + orderRow.index
                            focusPolicy: Qt.NoFocus
                            text: i18nc("@action:button", "Move Up")
                            icon.name: "arrow-up"
                            display: Controls.AbstractButton.IconOnly
                            Accessible.name: i18nc("@action:button %1 is a library name", "Move %1 up", orderRow.text)
                            Controls.ToolTip.text: i18nc("@info:tooltip", "Move up")
                            Controls.ToolTip.visible: hovered
                            Controls.ToolTip.delay: Kirigami.Units.toolTipDelay
                            enabled: orderRow.index > 0
                            onClicked: page.moveLibrary(orderRow.index, -1)
                        }
                        Controls.ToolButton {
                            objectName: "prefMoveDown_" + orderRow.index
                            focusPolicy: Qt.NoFocus
                            text: i18nc("@action:button", "Move Down")
                            icon.name: "arrow-down"
                            display: Controls.AbstractButton.IconOnly
                            Accessible.name: i18nc("@action:button %1 is a library name", "Move %1 down", orderRow.text)
                            Controls.ToolTip.text: i18nc("@info:tooltip", "Move down")
                            Controls.ToolTip.visible: hovered
                            Controls.ToolTip.delay: Kirigami.Units.toolTipDelay
                            enabled: orderRow.index < page.prefs.orderedViews.length - 1
                            onClicked: page.moveLibrary(orderRow.index, 1)
                        }
                    }
                }
            }
        }

        FormCard.FormHeader {
            title: i18nc("@title:group", "Libraries on Home")
        }
        FormCard.FormCard {
            FormCard.FormSectionText {
                text: page.isMalformed("MyMediaExcludes") ? page.malformedNote() : i18n("Checked libraries get no tile on Home. Hiding a library here does not remove access to it: it stays available in Movies, Shows, Favorites and Search")
            }
            Repeater {
                model: page.prefs.libraries
                delegate: LibraryCheckRow {
                    id: hideRow

                    required property var modelData

                    objectName: "prefMyMediaExclude_" + modelData.id
                    text: modelData.name
                    accessibleLabel: i18nc("@option:check %1 is a library name", "Hide %1 from the Home tiles", modelData.name)
                    enabled: !page.isMalformed("MyMediaExcludes")
                    checked: page.listsId(page.prefs.myMediaExcludes, modelData.id)
                    onToggled: page.prefs.setMyMediaExcluded(modelData.id, checked)
                }
            }
        }

        FormCard.FormHeader {
            title: i18nc("@title:group", "Recently Added rows on Home")
        }
        FormCard.FormCard {
            FormCard.FormSectionText {
                text: page.isMalformed("LatestItemsExcludes") ? page.malformedNote() : i18n("Checked libraries get no Recently Added row on Home. Their tiles, Continue Watching and Next Up are not affected")
            }
            Repeater {
                model: page.prefs.libraries
                delegate: LibraryCheckRow {
                    id: latestRow

                    required property var modelData

                    objectName: "prefLatestExclude_" + modelData.id
                    text: modelData.name
                    accessibleLabel: i18nc("@option:check %1 is a library name", "Leave %1 out of the Recently Added rows", modelData.name)
                    enabled: !page.isMalformed("LatestItemsExcludes")
                    checked: page.listsId(page.prefs.latestItemsExcludes, modelData.id)
                    onToggled: page.prefs.setLatestExcluded(modelData.id, checked)
                }
            }
            FormCard.FormSectionText {
                objectName: "prefUnavailableLibraries"
                visible: page.prefs.unavailableLibraryCount > 0
                text: i18np("The preferences of %1 library that is not available are kept", "The preferences of %1 libraries that are not available are kept", page.prefs.unavailableLibraryCount)
            }
        }

        FormCard.FormHeader {
            title: i18nc("@title:group", "Home sections set in Jellyfin")
        }
        FormCard.FormCard {
            FormCard.FormSectionText {
                text: i18n("Read only: Kinefin keeps its own Home layout")
            }
            FormCard.FormSectionText {
                objectName: "prefHomeSectionsStatus"
                visible: page.prefs.homeSectionsStatus !== UserConfigurationModel.HomeSectionsReady || page.prefs.homeSections.length === 0
                text: {
                    switch (page.prefs.homeSectionsStatus) {
                    case UserConfigurationModel.HomeSectionsUnavailable:
                        return i18n("The Jellyfin preview is not available");
                    case UserConfigurationModel.HomeSectionsReady:
                        return i18n("No sections are set");
                    }
                    return i18n("Loading…");
                }
            }
            Repeater {
                model: page.prefs.homeSections
                delegate: Controls.Label {
                    required property int index
                    required property int modelData

                    objectName: "prefHomeSection_" + index
                    Layout.fillWidth: true
                    Layout.leftMargin: Kirigami.Units.largeSpacing + Kirigami.Units.smallSpacing
                    Layout.rightMargin: Kirigami.Units.largeSpacing + Kirigami.Units.smallSpacing
                    Layout.topMargin: Kirigami.Units.smallSpacing
                    Layout.bottomMargin: Kirigami.Units.smallSpacing
                    textFormat: Text.PlainText
                    text: {
                        switch (modelData) {
                        case UserConfigurationModel.Libraries:
                            return i18nc("@item:intable Home section", "Libraries");
                        case UserConfigurationModel.ContinueWatching:
                            return i18nc("@item:intable Home section", "Continue Watching");
                        case UserConfigurationModel.NextUp:
                            return i18nc("@item:intable Home section", "Next Up");
                        case UserConfigurationModel.RecentlyAdded:
                            return i18nc("@item:intable Home section", "Recently Added");
                        }
                        return i18nc("@item:intable Home section", "Section not supported by Kinefin");
                    }
                }
            }
        }
    }

    footer: Controls.ToolBar {
        visible: page.prefs.loaded
        position: Controls.ToolBar.Footer

        contentItem: RowLayout {
            spacing: Kirigami.Units.largeSpacing

            Controls.BusyIndicator {
                Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
                Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                visible: page.saving
                running: visible
                Accessible.ignored: true
            }
            Controls.Label {
                visible: page.saving
                textFormat: Text.PlainText
                text: i18n("Saving…")
            }
            Item {
                Layout.fillWidth: true
            }
            Controls.Button {
                objectName: "prefsDiscardButton"

                text: i18nc("@action:button", "Discard")
                icon.name: "edit-undo"
                enabled: page.prefs.dirty && !page.busy
                onClicked: page.prefs.discard()
            }
            Controls.Button {
                objectName: "prefsSaveButton"

                text: i18nc("@action:button", "Save")
                icon.name: "document-save"
                enabled: page.prefs.canSave
                onClicked: page.prefs.save()
            }
        }
    }

    Kirigami.PromptDialog {
        id: leaveDialog
        objectName: "unsavedChangesDialog"

        // Runs once the user chose to leave
        property var proceed: null

        title: i18nc("@title:window", "Save Changes?")
        subtitle: i18n("The changes to your preferences are not saved")
        standardButtons: Kirigami.Dialog.Cancel
        customFooterActions: [
            Kirigami.Action {
                text: i18nc("@action:button", "Discard")
                icon.name: "edit-undo"
                onTriggered: {
                    const leave = leaveDialog.proceed;
                    leaveDialog.close();
                    page.prefs.discard();
                    if (leave)
                        leave();
                }
            },
            Kirigami.Action {
                text: i18nc("@action:button", "Save")
                icon.name: "document-save"
                enabled: page.prefs.canSave
                onTriggered: {
                    page.pendingLeave = leaveDialog.proceed;
                    leaveDialog.close();
                    page.prefs.save();
                }
            }
        ]
        onClosed: proceed = null
    }
}
