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

    title: i18n("Edit Metadata")
    preferredWidth: Kirigami.Units.gridUnit * 30 - Kirigami.Units.largeSpacing * 2
    standardButtons: Kirigami.Dialog.Cancel
    padding: Kirigami.Units.largeSpacing
    customFooterActions: [
        Kirigami.Action {
            objectName: "editMetadataSave"
            text: i18n("Save")
            icon.name: "document-save"
            enabled: dialog.acceptable && !dialog.saving
            onTriggered: dialog.save()
        }
    ]

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

    Kirigami.FormLayout {
        Layout.fillWidth: true

        Controls.TextField {
            id: nameField
            objectName: "editMetadataName"
            Kirigami.FormData.label: i18nc("@label:textbox", "Name:")
            Layout.fillWidth: true
            maximumLength: 512
            Accessible.name: i18n("Name")
        }
        Controls.TextArea {
            id: overviewField
            objectName: "editMetadataOverview"
            Kirigami.FormData.label: i18nc("@label:textbox", "Overview:")
            Layout.fillWidth: true
            // FormLayout copies only the width: the field carries its own height, fixed so a long text neither moves Year nor resizes the dialog
            height: Kirigami.Units.gridUnit * 6
            implicitHeight: Kirigami.Units.gridUnit * 6
            wrapMode: Text.Wrap
            Accessible.name: i18n("Overview")
        }
        Controls.SpinBox {
            id: yearField
            objectName: "editMetadataYear"
            Kirigami.FormData.label: i18nc("@label:spinbox", "Year:")
            from: 0
            to: 9999
            Accessible.name: i18n("Year")
        }
        Controls.TextField {
            id: ratingField
            objectName: "editMetadataRating"
            Kirigami.FormData.label: i18nc("@label:textbox", "Official rating:")
            Layout.fillWidth: true
            maximumLength: 64
            Accessible.name: i18n("Official rating")
        }
        Controls.TextField {
            id: genresField
            objectName: "editMetadataGenres"
            Kirigami.FormData.label: i18nc("@label:textbox", "Genres:")
            Layout.fillWidth: true
            placeholderText: i18n("Drama, Thriller")
            Accessible.name: i18n("Genres")
        }
        Controls.Label {
            Layout.fillWidth: true
            visible: !dialog.overviewValid
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: i18n("The overview is limited to 16384 characters")
        }
        Controls.Label {
            Layout.fillWidth: true
            visible: !dialog.genresValid
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: i18n("Use at most 20 genres, each of 128 characters at most")
        }
    }
}
