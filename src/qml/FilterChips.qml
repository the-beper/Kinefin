// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The filters of a list as chips with a remove button each, and Clear Filters for all of them (not the search, the sort or the library). The dialog builds the chips from the filters of its model and takes one off; the bar shows only while the list has a chip or still resolves saved filters. The chips are not `Kirigami.Chip`: it reads the text format of its label by guessing, and these labels are server text
Flow {
    id: bar
    objectName: "filterChips"

    // The dialog of the list: its `chips` and `removeChip()`
    required property FilterDialog dialog
    // The list the filters are of (a LibraryItemsModel or the FavoritesModel), null for none: the bar is hidden then
    property var list: null
    // Only a library resolves saved filters
    readonly property bool resolving: list?.resolvingFacets ?? false
    // A page that offers Clear Filters itself (its placeholder when the filters leave nothing) hides this button meanwhile, so that one window has one
    property bool clearShown: true

    Layout.fillWidth: true
    Layout.leftMargin: Kirigami.Units.largeSpacing
    Layout.rightMargin: Kirigami.Units.largeSpacing
    Layout.topMargin: Kirigami.Units.smallSpacing
    visible: list !== null && (dialog.chips.length > 0 || resolving)
    spacing: Kirigami.Units.smallSpacing

    Repeater {
        model: bar.dialog.chips

        delegate: Rectangle {
            id: chip

            required property var modelData
            required property int index

            objectName: "filterChip_" + index
            implicitWidth: chipRow.implicitWidth + Kirigami.Units.smallSpacing * 2
            implicitHeight: chipRow.implicitHeight + Kirigami.Units.smallSpacing
            radius: height / 2
            color: Kirigami.Theme.alternateBackgroundColor
            border.width: 1
            border.color: Qt.alpha(Kirigami.Theme.textColor, 0.2)

            RowLayout {
                id: chipRow
                anchors.centerIn: parent
                spacing: 0

                // Genres, tags and ratings come from the server: plain text only
                Controls.Label {
                    Layout.leftMargin: Kirigami.Units.smallSpacing
                    Layout.maximumWidth: Kirigami.Units.gridUnit * 14
                    elide: Text.ElideRight
                    textFormat: Text.PlainText
                    text: chip.modelData.label
                }

                // The label is server text for some chips: only the accessible name carries it, the control text is the fixed tooltip text
                Controls.ToolButton {
                    objectName: "filterChipRemove_" + chip.index
                    Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium + Kirigami.Units.smallSpacing
                    Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium + Kirigami.Units.smallSpacing
                    padding: 0
                    display: Controls.AbstractButton.IconOnly
                    icon.name: "window-close-symbolic"
                    icon.width: Kirigami.Units.iconSizes.small
                    icon.height: Kirigami.Units.iconSizes.small
                    text: i18n("Remove this filter")
                    Accessible.name: i18nc("accessible name of the button that removes one filter, %1 is the filter", "Remove the filter %1", chip.modelData.label)
                    Controls.ToolTip.text: text
                    Controls.ToolTip.visible: hovered || visualFocus
                    Controls.ToolTip.delay: Kirigami.Units.toolTipDelay
                    onClicked: bar.dialog.removeChip(chip.modelData)
                }
            }
        }
    }

    Controls.Label {
        objectName: "resolvingFiltersLabel"
        visible: bar.resolving
        opacity: 0.7
        text: i18n("Applying the saved filters…")
    }

    Controls.Button {
        objectName: "clearFiltersButton"
        visible: bar.clearShown
        flat: true
        icon.name: "edit-clear-all"
        text: i18n("Clear Filters")
        onClicked: bar.list.clearFilters()
    }
}
