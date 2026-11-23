// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The filters kept under a name for one kind of list (movies, series, episodes, collections): choose one to put it on the list, save the filters of the Filters dialog's draft under a name, rename or delete one (Delete can be undone). The names are the user's own text, shown as plain text; the filters themselves keep tags and ratings by digest in the browse state file
Kirigami.Dialog {
    id: dialog
    objectName: "savedFiltersDialog"

    // The Filters dialog this one belongs to: its draft is what Save keeps and its list is what a set is put on
    required property var filters

    readonly property ConnectionController connection: filters.connection
    readonly property string kind: filters.savedKind
    // The sets of the kind, read again whenever one is saved, renamed or removed
    readonly property var sets: {
        connection?.savedFiltersRevision;
        return connection && kind.length > 0 ? connection.savedFilters(kind) : [];
    }
    // What the last save, rename or delete said, and the name a Replace would write over
    property string problem: ""
    property string replaceName: ""
    // A set was deleted a moment ago and Undo is on offer
    property bool undoOffered: false

    title: i18nc("@title:window", "Saved Filters")
    preferredWidth: Kirigami.Units.gridUnit * 22
    standardButtons: Kirigami.Dialog.Close

    header: DialogTitleBar {
        title: dialog.title
        onCloseRequested: dialog.close()
    }

    function setProblem(text, replace) {
        problem = text;
        replaceName = replace ?? "";
        // Any other result takes the place of the offer
        if (text.length > 0)
            undoOffered = false;
    }

    // Puts a set on the list and leaves both dialogs: the list shows what the set says
    function applySet(name) {
        if (connection.applySavedFilters(filters.list, name)) {
            close();
            filters.close();
        } else {
            setProblem(i18n("These filters could not be applied: close the Filters dialog and open it again"));
        }
    }

    function save(replace) {
        const name = nameField.text.trim();
        switch (connection.saveFilters(kind, name, filters.draft, replace)) {
        case "saved":
        case "replaced":
            nameField.text = "";
            undoOffered = false;
            setProblem("");
            break;
        case "exists":
            setProblem(i18n("Filters with this name are saved already: choose another name or replace them"), name);
            break;
        case "full":
            setProblem(i18n("There is room for 50 saved filters: delete one first"));
            break;
        default:
            setProblem(i18n("These filters could not be saved under this name: use up to 40 characters, none of them a line break"));
        }
    }

    function remove(name) {
        if (!connection.removeSavedFilters(kind, name))
            return;
        // The rows move up: a rename in progress would be on another set
        renaming = -1;
        setProblem("");
        // The window's notification is out of reach of a modal dialog's keys: the offer to undo is a box in the dialog
        undoOffered = true;
        Qt.callLater(() => (sets.length > 0 ? list : nameField).forceActiveFocus(Qt.OtherFocusReason));
    }

    // The row being renamed (an index of `sets`), -1 for none: the name turns into a field in its place
    property int renaming: -1

    function askRename(name) {
        renaming = sets.findIndex(entry => entry.name === name);
    }

    function cancelRename() {
        renaming = -1;
        Qt.callLater(() => list.forceActiveFocus(Qt.OtherFocusReason));
    }

    function commitRename(newName) {
        const original = sets[renaming]?.name ?? "";
        const trimmed = newName.trim();
        if (original.length === 0 || trimmed.length === 0 || trimmed === original) {
            cancelRename();
            return;
        }
        switch (connection.renameSavedFilters(kind, original, trimmed)) {
        case "renamed":
            undoOffered = false;
            setProblem("");
            break;
        case "exists":
            setProblem(i18n("Filters with this name are saved already: choose another name"));
            break;
        default:
            setProblem(i18n("The name cannot be used: use up to 40 characters, none of them a line break"));
        }
        cancelRename();
    }

    onClosed: {
        renaming = -1;
        undoOffered = false;
    }
    onOpened: {
        setProblem("");
        undoOffered = false;
        nameField.text = "";
        (sets.length > 0 ? list : nameField).forceActiveFocus(Qt.PopupFocusReason);
    }

    ColumnLayout {
        spacing: Kirigami.Units.smallSpacing

        Kirigami.PlaceholderMessage {
            objectName: "savedFiltersEmpty"
            Layout.fillWidth: true
            Layout.topMargin: Kirigami.Units.largeSpacing
            Layout.bottomMargin: Kirigami.Units.largeSpacing
            visible: dialog.sets.length === 0
            icon.name: "bookmarks"
            text: i18n("Nothing saved for this list yet")
            explanation: i18n("Choose filters, then save them here under a name")
        }

        ListView {
            id: list
            objectName: "savedFiltersList"

            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, Kirigami.Units.gridUnit * 14)
            visible: dialog.sets.length > 0
            clip: true
            model: dialog.sets
            activeFocusOnTab: true
            keyNavigationEnabled: true
            currentIndex: 0
            Accessible.role: Accessible.List
            Accessible.name: dialog.title

            Controls.ScrollBar.vertical: Controls.ScrollBar {}

            Keys.onReturnPressed: if (currentIndex >= 0) dialog.applySet(dialog.sets[currentIndex].name)
            Keys.onEnterPressed: if (currentIndex >= 0) dialog.applySet(dialog.sets[currentIndex].name)
            Keys.onDeletePressed: if (currentIndex >= 0) dialog.remove(dialog.sets[currentIndex].name)
            Keys.onPressed: event => {
                if (event.key === Qt.Key_F2 && currentIndex >= 0) {
                    dialog.askRename(dialog.sets[currentIndex].name);
                    event.accepted = true;
                }
            }

            delegate: Controls.ItemDelegate {
                id: row

                required property var modelData
                required property int index
                readonly property bool editing: dialog.renaming === index

                objectName: "savedFilter_" + index
                width: ListView.view.width
                highlighted: ListView.isCurrentItem && ListView.view.activeFocus && !editing
                // The set's own name, as the user typed it: the row has no text of its own, so no mnemonic or markup applies
                Accessible.name: modelData.name
                onClicked: if (!editing) dialog.applySet(modelData.name)

                contentItem: RowLayout {
                    spacing: Kirigami.Units.smallSpacing

                    Controls.Label {
                        Layout.fillWidth: true
                        visible: !row.editing
                        elide: Text.ElideRight
                        textFormat: Text.PlainText
                        text: row.modelData.name
                        color: row.highlighted ? Kirigami.Theme.highlightedTextColor : Kirigami.Theme.textColor
                    }
                    // The name as an editable field in its place: Return keeps it, Escape or leaving it does not. The user's own text in a field is plain text
                    Controls.TextField {
                        id: editor
                        objectName: "savedFilterRenameField"
                        Layout.fillWidth: true
                        visible: row.editing
                        maximumLength: 40
                        text: row.modelData.name
                        Accessible.name: i18nc("@label:textbox", "Name")
                        onVisibleChanged: if (visible) {
                            text = row.modelData.name;
                            forceActiveFocus(Qt.OtherFocusReason);
                            selectAll();
                        }
                        onAccepted: dialog.commitRename(text)
                        // Escape ends the rename, not the dialog
                        Keys.onShortcutOverride: event => {
                            if (event.key === Qt.Key_Escape)
                                event.accepted = true;
                        }
                        Keys.onEscapePressed: dialog.cancelRename()
                    }
                    Controls.ToolButton {
                        objectName: "savedFilterRename_" + row.index
                        visible: !row.editing
                        text: i18nc("@action:button", "Rename Saved Filters…")
                        icon.name: "edit-rename"
                        display: Controls.AbstractButton.IconOnly
                        Accessible.name: i18nc("@action:button %1 is the name", "Rename %1", row.modelData.name)
                        Controls.ToolTip.text: text
                        Controls.ToolTip.visible: hovered || visualFocus
                        Controls.ToolTip.delay: Kirigami.Units.toolTipDelay
                        onActiveFocusChanged: if (activeFocus) list.currentIndex = row.index
                        onClicked: dialog.askRename(row.modelData.name)
                    }
                    Controls.ToolButton {
                        objectName: "savedFilterDelete_" + row.index
                        visible: !row.editing
                        text: i18nc("@action:button", "Delete Saved Filters")
                        icon.name: "edit-delete"
                        display: Controls.AbstractButton.IconOnly
                        Accessible.name: i18nc("@action:button %1 is the name", "Delete %1", row.modelData.name)
                        Controls.ToolTip.text: text
                        Controls.ToolTip.visible: hovered || visualFocus
                        Controls.ToolTip.delay: Kirigami.Units.toolTipDelay
                        onActiveFocusChanged: if (activeFocus) list.currentIndex = row.index
                        onClicked: dialog.remove(row.modelData.name)
                    }
                }
            }
        }

        MessageBox {
            objectName: "savedFiltersRemoved"
            Layout.topMargin: Kirigami.Units.largeSpacing
            visible: dialog.undoOffered
            type: Kirigami.MessageType.Information
            text: i18n("Saved filters deleted")
            actions: [
                Kirigami.Action {
                    text: i18nc("@action:button", "Undo")
                    icon.name: "edit-undo"
                    onTriggered: {
                        dialog.undoOffered = false;
                        dialog.connection.undoRemoveSavedFilters();
                    }
                }
            ]
        }

        // Close to the field the message is about
        MessageBox {
            objectName: "savedFiltersProblem"
            Layout.topMargin: Kirigami.Units.largeSpacing
            visible: dialog.problem.length > 0
            type: Kirigami.MessageType.Warning
            text: dialog.problem
            actions: [
                Kirigami.Action {
                    text: i18nc("@action:button", "Replace")
                    icon.name: "document-replace"
                    visible: dialog.replaceName.length > 0
                    onTriggered: dialog.save(true)
                }
            ]
        }

        Kirigami.Separator {
            Layout.fillWidth: true
        }

        // The filters of the Filters dialog as they stand now, kept under a name
        Controls.Label {
            Layout.fillWidth: true
            Layout.leftMargin: Kirigami.Units.smallSpacing
            text: i18nc("@label:textbox", "Save the current filters as:")
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Kirigami.Units.smallSpacing
            Layout.rightMargin: Kirigami.Units.smallSpacing
            Layout.bottomMargin: Kirigami.Units.smallSpacing
            spacing: Kirigami.Units.smallSpacing

            // The user's own text in an editable field: a TextField shows it as plain text
            Controls.TextField {
                id: nameField
                objectName: "savedFilterName"
                Layout.fillWidth: true
                maximumLength: 40
                Accessible.name: i18nc("@label:textbox", "Name")
                onTextChanged: if (dialog.problem.length > 0) dialog.setProblem("")
                onAccepted: if (saveButton.enabled) dialog.save(false)
            }
            Controls.Button {
                id: saveButton
                objectName: "savedFilterSave"
                text: i18nc("@action:button", "Save")
                icon.name: "document-save"
                enabled: nameField.text.trim().length > 0 && !dialog.filters.draftIsEmpty
                Controls.ToolTip.text: dialog.filters.draftIsEmpty ? i18n("Choose some filters first") : ""
                Controls.ToolTip.visible: hovered && dialog.filters.draftIsEmpty
                Controls.ToolTip.delay: Kirigami.Units.toolTipDelay
                onClicked: dialog.save(false)
            }
        }
    }
}
