// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.Dialog {
    id: dialog
    objectName: "genreFilterDialog"

    required property LibraryItemsModel library

    // A plain array copy: the delegates bind to it, and the model's list is never changed in place
    readonly property var selectedIds: Array.from(library.genreIds)

    title: i18n("Filter by Genre")
    padding: 0
    standardButtons: Kirigami.Dialog.Close
    customFooterActions: [
        Kirigami.Action {
            text: i18n("Clear")
            icon.name: "edit-clear-all"
            enabled: dialog.selectedIds.length > 0
            onTriggered: dialog.library.genreIds = []
        }
    ]

    function toggle(genreId) {
        const ids = Array.from(library.genreIds);
        const at = ids.indexOf(genreId);
        if (at >= 0)
            ids.splice(at, 1);
        else
            ids.push(genreId);
        library.genreIds = ids;
    }

    onOpened: list.forceActiveFocus(Qt.PopupFocusReason)

    ListView {
        id: list
        objectName: "genreOptionsList"

        implicitWidth: Kirigami.Units.gridUnit * 20
        implicitHeight: Math.min(contentHeight, Kirigami.Units.gridUnit * 20)
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        activeFocusOnTab: true
        keyNavigationEnabled: true
        currentIndex: 0
        model: dialog.library.genreOptions
        Accessible.role: Accessible.List
        Accessible.name: i18n("Genres")

        Controls.ScrollBar.vertical: Controls.ScrollBar {}

        // The rows are not tab stops: the list is, and it toggles the current genre
        function toggleCurrent() {
            if (list.currentItem)
                dialog.toggle(list.currentItem.genreId);
        }
        Keys.onReturnPressed: toggleCurrent()
        Keys.onEnterPressed: toggleCurrent()
        Keys.onSpacePressed: toggleCurrent()

        delegate: Controls.ItemDelegate {
            id: option
            objectName: "genreOption_" + genreId

            required property int index
            required property string genreId
            required property string name
            readonly property bool selected: dialog.selectedIds.indexOf(genreId) >= 0

            width: ListView.view.width
            focusPolicy: Qt.NoFocus
            highlighted: ListView.isCurrentItem && ListView.view.activeFocus
            onClicked: {
                ListView.view.currentIndex = index;
                dialog.toggle(genreId);
            }

            // The name is server text: the control has no text of its own, so no mnemonic or markup applies
            Accessible.role: Accessible.CheckBox
            Accessible.name: name
            Accessible.checkable: true
            Accessible.checked: selected

            contentItem: RowLayout {
                spacing: Kirigami.Units.largeSpacing

                Controls.CheckBox {
                    focusPolicy: Qt.NoFocus
                    checked: option.selected
                    Accessible.ignored: true
                    // A click on the box itself must also go through the model, then the box follows the model again
                    onToggled: {
                        dialog.toggle(option.genreId);
                        checked = Qt.binding(() => option.selected);
                    }
                }

                Controls.Label {
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    textFormat: Text.PlainText
                    text: option.name
                }
            }
        }
    }
}
