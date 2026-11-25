// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The metadata fields Kinefin edits: name, overview, year, official rating and genres. Only the changed fields are sent
Kirigami.Dialog {
    id: dialog
    objectName: "editMetadataDialog"

    required property ConnectionController connection
    required property ItemDetailsModel details
    // The window shows the notifications
    required property var shell

    // The write is acknowledged by the page's Connections, which reload the item and notify; the dialog closes only once the server answers
    property bool saving: false

    readonly property string newName: nameField.text.trim()
    readonly property var newGenres: genresField.text.split(",").map(value => value.trim()).filter(value => value.length > 0)
    readonly property bool genresChanged: newGenres.join("\u0000") !== details.genres.join("\u0000")
    // The same limits the client enforces: a field over them would be refused without telling why
    readonly property bool overviewValid: overviewField.text.length <= 16384
    readonly property bool genresValid: newGenres.length <= 20
                                          && newGenres.every(value => value.length <= 128)
    readonly property bool changed: newName !== details.name
                                    || overviewField.text !== details.overview
                                    || yearField.value !== details.year
                                    || ratingField.text.trim() !== details.officialRating
                                    || genresChanged
    readonly property bool acceptable: newName.length > 0 && changed && overviewValid
                                       && genresValid

    // One size in every state: the overview takes the room the other fields leave, and the dialog must not move when a warning shows. The room the window leaves is a limit too, and the opening animation starts the dialog two grid units lower, so it must not reach the window edge while it moves
    readonly property real totalHeight: Math.min(Kirigami.Units.gridUnit * 28, absoluteMaximumHeight - Kirigami.Units.gridUnit * 4)
    readonly property real bodyHeight: Math.max(Kirigami.Units.gridUnit * 8, totalHeight - implicitHeaderHeight - implicitFooterHeight)

    title: i18n("Edit Metadata")
    preferredWidth: Kirigami.Units.gridUnit * 30
    standardButtons: Kirigami.Dialog.Cancel
    customFooterActions: [
        Kirigami.Action {
            objectName: "editMetadataSave"
            text: i18n("Save")
            icon.name: "document-save"
            enabled: dialog.acceptable && !dialog.saving
            onTriggered: dialog.save()
        }
    ]

    header: DialogTitleBar {
        title: dialog.title
        onCloseRequested: dialog.close()
    }

    function sameItem(itemId) {
        return itemId.toLowerCase().replace(/-/g, "") === dialog.details.itemId.toLowerCase().replace(/-/g, "");
    }

    Connections {
        target: dialog.connection
        function onItemUpdated(itemId) {
            if (!dialog.sameItem(itemId))
                return;
            dialog.saving = false;
            dialog.close();
        }
        function onItemUpdateFailed(itemId, status) {
            if (dialog.sameItem(itemId))
                dialog.saving = false;
        }
    }

    function save() {
        if (!acceptable || saving)
            return;
        const fields = {};
        if (newName !== details.name)
            fields.Name = newName;
        if (overviewField.text !== details.overview)
            fields.Overview = overviewField.text;
        if (yearField.value !== details.year)
            fields.ProductionYear = yearField.value;
        if (ratingField.text.trim() !== details.officialRating)
            fields.OfficialRating = ratingField.text.trim();
        if (genresChanged)
            fields.Genres = newGenres;
        if (!connection.updateItem(details.itemId, fields)) {
            shell.showPassiveNotification(i18n("The metadata could not be saved: wait for the changes on their way and try again"));
            return;
        }
        saving = true;
    }

    onAboutToShow: {
        saving = false;
        nameField.text = details.name;
        overviewField.text = details.overview;
        yearField.value = details.year;
        ratingField.text = details.officialRating;
        genresField.text = details.genres.join(", ");
    }

    Item {
        implicitHeight: dialog.bodyHeight

        ColumnLayout {
            anchors {
                fill: parent
                margins: Kirigami.Units.largeSpacing * 2
            }
            spacing: Kirigami.Units.largeSpacing

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Kirigami.Units.smallSpacing

                Controls.Label {
                    text: i18nc("@label:textbox", "Name:")
                }
                Controls.TextField {
                    id: nameField
                    objectName: "editMetadataName"
                    Layout.fillWidth: true
                    maximumLength: 512
                    Accessible.name: i18n("Name")
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: Kirigami.Units.smallSpacing

                Controls.Label {
                    text: i18nc("@label:textbox", "Overview:")
                }
                // The text scrolls inside the box: a long overview neither moves the fields under it nor resizes the dialog
                Controls.ScrollView {
                    objectName: "editMetadataOverviewBox"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.preferredHeight: Kirigami.Units.gridUnit * 4
                    Layout.minimumHeight: Kirigami.Units.gridUnit * 4

                    Controls.TextArea {
                        id: overviewField
                        objectName: "editMetadataOverview"
                        wrapMode: Text.Wrap
                        Accessible.name: i18n("Overview")
                        // Tab leaves the field instead of typing a tab
                        KeyNavigation.priority: KeyNavigation.BeforeItem
                        KeyNavigation.tab: yearField
                        KeyNavigation.backtab: nameField
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Kirigami.Units.largeSpacing

                ColumnLayout {
                    Layout.alignment: Qt.AlignTop
                    spacing: Kirigami.Units.smallSpacing

                    Controls.Label {
                        text: i18nc("@label:spinbox", "Year:")
                    }
                    Controls.SpinBox {
                        id: yearField
                        objectName: "editMetadataYear"
                        from: 0
                        to: 9999
                        Accessible.name: i18n("Year")
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignTop
                    spacing: Kirigami.Units.smallSpacing

                    Controls.Label {
                        text: i18nc("@label:textbox", "Official rating:")
                    }
                    Controls.TextField {
                        id: ratingField
                        objectName: "editMetadataRating"
                        Layout.fillWidth: true
                        maximumLength: 64
                        Accessible.name: i18n("Official rating")
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Kirigami.Units.smallSpacing

                Controls.Label {
                    text: i18nc("@label:textbox", "Genres:")
                }
                Controls.TextField {
                    id: genresField
                    objectName: "editMetadataGenres"
                    Layout.fillWidth: true
                    placeholderText: i18n("Drama, Thriller")
                    Accessible.name: i18n("Genres")
                }
            }

            // One box at a time: the overview first
            MessageBox {
                objectName: "editMetadataInvalid"
                Layout.fillWidth: true
                Layout.topMargin: Kirigami.Units.largeSpacing
                visible: !dialog.overviewValid || !dialog.genresValid
                type: Kirigami.MessageType.Warning
                text: !dialog.overviewValid ? i18n("The overview is limited to 16384 characters") : i18n("Use at most 20 genres, each of 128 characters at most")
            }
        }
    }
}
