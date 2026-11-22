// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The images of one item: upload a local JPEG, PNG or WebP or delete the current one
Kirigami.Dialog {
    id: dialog
    objectName: "editImagesDialog"

    required property ConnectionController connection
    required property ItemDetailsModel details
    // The window shows the notifications
    required property var shell

    property string pendingType: ""
    property int pendingIndex: -1
    property string deleteType: ""
    property int deleteIndex: -1

    title: i18n("Edit Images")
    preferredWidth: Kirigami.Units.gridUnit * 24
    standardButtons: Kirigami.Dialog.Cancel
    padding: Kirigami.Units.largeSpacing

    function choose(imageType, index) {
        pendingType = imageType;
        pendingIndex = index;
        imageDialog.open();
    }

    function confirmDelete(imageType, index) {
        // One dialog at a time: the confirmation takes the edit dialog's place
        dialog.close();
        deleteType = imageType;
        deleteIndex = index;
        deleteDialog.open();
    }

    function removeImage() {
        deleteDialog.close();
        if (!dialog.connection.deleteImage(dialog.details.itemId, deleteType, deleteIndex))
            dialog.shell.showPassiveNotification(i18n("The image could not be deleted: wait for the changes on their way and try again"));
    }

    Connections {
        target: dialog.connection
        function onItemImageUpdated(itemId, imageType, index) {
            if (itemId !== dialog.details.itemId)
                return;
            dialog.details.reload();
            dialog.shell.showPassiveNotification(i18n("Image updated"));
        }
        function onItemImageUpdateFailed(itemId, imageType, index, status) {
            if (itemId !== dialog.details.itemId)
                return;
            dialog.shell.showPassiveNotification(i18n("The image could not be saved: check the file and the connection to the server"));
        }
    }

    FileDialog {
        id: imageDialog
        objectName: "editImageFileDialog"
        title: i18n("Choose an Image")
        nameFilters: [i18n("Images (*.jpg *.jpeg *.png *.webp)")]
        onAccepted: {
            if (!dialog.connection.uploadImage(dialog.details.itemId, dialog.pendingType,
                                               dialog.pendingIndex, selectedFile))
                dialog.shell.showPassiveNotification(i18n("The image could not be sent: choose a JPEG, PNG, or WebP under 20 MiB"));
        }
    }

    Kirigami.PromptDialog {
        id: deleteDialog
        objectName: "deleteImageDialog"

        title: i18n("Delete Image")
        standardButtons: Kirigami.Dialog.Cancel
        customFooterActions: [
            Kirigami.Action {
                objectName: "confirmDeleteImage"
                text: i18n("Delete")
                icon.name: "edit-delete"
                onTriggered: dialog.removeImage()
            }
        ]

        Controls.Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: i18n("Delete this image from the server? It cannot be recovered")
        }
    }

    ColumnLayout {
        Layout.fillWidth: true

        Controls.Label {
            Layout.fillWidth: true
            opacity: 0.7
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: i18n("JPEG, PNG, or WebP, at most 20 MiB")
        }

        Kirigami.FormLayout {
            Layout.fillWidth: true

            RowLayout {
                Kirigami.FormData.label: i18nc("@label image type", "Primary:")
                Controls.Button {
                    objectName: "editImageUpload_Primary"
                    text: i18n("Upload…")
                    icon.name: "document-open"
                    Accessible.name: i18n("Upload Primary Image")
                    onClicked: dialog.choose("Primary", -1)
                }
                Controls.Button {
                    objectName: "editImageDelete_Primary"
                    text: i18n("Delete…")
                    icon.name: "edit-delete"
                    Accessible.name: i18n("Delete Primary Image")
                    visible: dialog.details.posterUrl.toString().length > 0
                    onClicked: dialog.confirmDelete("Primary", -1)
                }
            }

            RowLayout {
                Kirigami.FormData.label: i18nc("@label image type", "Backdrop:")
                Controls.Button {
                    objectName: "editImageUpload_Backdrop"
                    text: i18n("Upload…")
                    icon.name: "document-open"
                    Accessible.name: i18n("Upload Backdrop")
                    onClicked: dialog.choose("Backdrop", 0)
                }
                Controls.Button {
                    objectName: "editImageDelete_Backdrop"
                    text: i18n("Delete…")
                    icon.name: "edit-delete"
                    Accessible.name: i18n("Delete Backdrop")
                    visible: dialog.details.backdropUrl.toString().length > 0
                    onClicked: dialog.confirmDelete("Backdrop", 0)
                }
            }

            RowLayout {
                Kirigami.FormData.label: i18nc("@label image type", "Logo:")
                Controls.Button {
                    objectName: "editImageUpload_Logo"
                    text: i18n("Upload…")
                    icon.name: "document-open"
                    Accessible.name: i18n("Upload Logo")
                    onClicked: dialog.choose("Logo", -1)
                }
                Controls.Button {
                    objectName: "editImageDelete_Logo"
                    text: i18n("Delete…")
                    icon.name: "edit-delete"
                    Accessible.name: i18n("Delete Logo")
                    visible: dialog.details.logoUrl.toString().length > 0
                    onClicked: dialog.confirmDelete("Logo", -1)
                }
            }
        }
    }
}
