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
    // A line over the whole bar, as the bars at the top have one under them
    background: Item {
        Kirigami.Separator {
            anchors {
                left: parent.left
                right: parent.right
                top: parent.top
            }
            Accessible.ignored: true
        }
    }

    // Ctrl+S saves while the bar shows, as its Save button does
    Shortcut {
        objectName: "prefsSaveShortcut"
        sequences: [StandardKey.Save]
        enabled: bar.visible && bar.prefs.canSave
        onActivated: bar.prefs.save()
    }

    contentItem: RowLayout {
        spacing: Kirigami.Units.largeSpacing

        Controls.BusyIndicator {
            FadeInOnShow {}
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
            icon.name: "edit-delete"
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
