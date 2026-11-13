// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The Save bar of the User entry of Settings: its own file because a bound inline component cannot be instantiated from another file
Controls.ToolBar {
    id: bar

    required property UserConfigurationModel prefs

    readonly property bool saving: prefs.state === UserConfigurationModel.Saving || prefs.state === UserConfigurationModel.Verifying
    readonly property bool busy: saving || prefs.state === UserConfigurationModel.Loading

    visible: prefs.loaded
    position: Controls.ToolBar.Footer

    contentItem: RowLayout {
        spacing: Kirigami.Units.largeSpacing

        Controls.BusyIndicator {
            Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
            Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
            visible: bar.saving
            running: visible
            Accessible.ignored: true
        }
        Controls.Label {
            visible: bar.saving
            textFormat: Text.PlainText
            text: i18n("Saving…")
        }
        Controls.Label {
            objectName: "prefsUnsavedLabel"
            visible: !bar.saving && bar.prefs.dirty
            textFormat: Text.PlainText
            text: i18n("Unsaved changes")
        }
        Item {
            Layout.fillWidth: true
        }
        Controls.Button {
            objectName: "prefsDiscardButton"

            text: i18nc("@action:button", "Discard")
            icon.name: "edit-undo"
            enabled: bar.prefs.dirty && !bar.busy
            onClicked: bar.prefs.discard()
        }
        Controls.Button {
            objectName: "prefsSaveButton"

            text: i18nc("@action:button", "Save")
            icon.name: "document-save"
            enabled: bar.prefs.canSave
            onClicked: bar.prefs.save()
        }
    }
}
