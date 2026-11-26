// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// A backdrop upload adds an image after the others, any other type is replaced, and the details page reads the item again after every write
Kirigami.ScrollablePage {
    id: page
    objectName: "editImagesPage"

    // The darker View background on every page: a page without a list (a plain flickable) would get the lighter Window one, and the sections would not match
    Kirigami.Theme.colorSet: Kirigami.Theme.View
    Kirigami.Theme.inherit: false

    required property ConnectionController connection
    required property string itemId
    // The item's name as the caller knew it: server text, shown as plain text only
    property string itemName: ""

    // The model is a child of this page and starts loading at once
    readonly property EditImagesModel imageModel: connection.createEditImages(itemId, page)
    readonly property bool loading: imageModel?.status === EditImagesModel.Loading
    readonly property bool failed: imageModel?.status === EditImagesModel.Failed
    // A write of this page is on its way: the buttons wait for the answer, as the images they address are about to change
    property bool writing: false
    property string writeKind: ""
    property string uploadType: ""
    property string deleteType: ""
    property int deleteIndex: -1
    // Set when the keyboard was asked for before the list had a first row
    property bool focusWanted: false

    title: i18n("Edit Images")
    // The row of the page carries the way back and the name, as on the Movies and Shows tabs
    globalToolBarStyle: Kirigami.ApplicationHeaderStyle.None
    // Without Kirigami's title row the page names itself to a screen reader
    Accessible.role: Accessible.Pane
    Accessible.name: title

    // The Upload button of the first type takes the keys as soon as the page shows
    function focusPrimary() {
        const first = list.itemAtIndex(0);
        if (first)
            first.uploadButton.forceActiveFocus(Qt.TabFocusReason);
        else
            focusWanted = true;
    }

    // What the main menu's Refresh calls while this page is in front
    function refreshPage() {
        if (imageModel && !loading)
            imageModel.reload();
    }

    function sameId(first, second) {
        return first.replace(/-/g, "").toLowerCase() === second.replace(/-/g, "").toLowerCase();
    }

    function notify(text) {
        Window.window?.showPassiveNotification(text);
    }

    function dimensions(image) {
        return image.width > 0 && image.height > 0 ? i18nc("@info image size in pixels, width then height", "%1 × %2", String(image.width), String(image.height)) : "";
    }

    function fileSize(image) {
        return image.size > 0 ? Qt.locale().formattedDataSize(image.size, 1) : "";
    }

    function searchOnline(type) {
        Window.window.openRemoteImages(itemId, itemName, type, page);
    }

    function beginWrite(kind) {
        writeKind = kind;
        writing = true;
    }

    // An upload to a type with several images adds one after the others, any other type is replaced
    function choose(type) {
        uploadType = type;
        imageDialog.open();
    }

    function upload(file) {
        if (connection.uploadImage(itemId, uploadType, -1, file))
            beginWrite("upload");
        else
            notify(i18n("The image could not be sent: choose a JPEG, PNG, or WebP of at most 20 MiB"));
    }

    function confirmDelete(type, index) {
        deleteType = type;
        deleteIndex = index;
        deleteDialog.open();
    }

    function removeImage() {
        deleteDialog.close();
        if (connection.deleteImage(itemId, deleteType, deleteIndex))
            beginWrite("delete");
        else
            notify(i18n("The image could not be deleted: wait for the changes on their way and try again"));
    }

    function moveImage(type, index, newIndex) {
        if (connection.moveImage(itemId, type, index, newIndex))
            beginWrite("move");
        else
            notify(i18n("The image could not be moved: wait for the changes on their way and try again"));
    }

    Connections {
        target: page.connection
        function onItemImageUpdated(itemId, imageType, index) {
            if (!page.sameId(itemId, page.itemId) || !page.writing)
                return;
            page.writing = false;
            page.notify(i18n("Image updated"));
        }
        function onItemImageUpdateFailed(itemId, imageType, index, status) {
            if (!page.sameId(itemId, page.itemId) || !page.writing)
                return;
            page.writing = false;
            if (page.writeKind === "delete")
                page.notify(i18n("The image could not be deleted: check the connection to the server and try again"));
            else if (page.writeKind === "move")
                page.notify(i18n("The image could not be moved: check the connection to the server and try again"));
            else
                page.notify(i18n("The image could not be saved: check the file and the connection to the server"));
        }
    }

    ImageTypes {
        id: types
    }

    // A button that scrolls itself into view when the keyboard reaches it
    component ActionButton: Controls.Button {
        id: actionButton
        onVisualFocusChanged: {
            if (visualFocus)
                Spatial.reveal(actionButton);
        }
    }

    // An icon button with its name as a tooltip
    component IconButton: Controls.ToolButton {
        id: iconButton
        display: Controls.AbstractButton.IconOnly
        onVisualFocusChanged: {
            if (visualFocus)
                Spatial.reveal(iconButton);
        }

        PlainToolTip {
            visible: (iconButton.hovered || iconButton.visualFocus) && iconButton.text.length > 0
            text: iconButton.text
        }
    }

    header: ColumnLayout {
        spacing: 0

        PageToolbar {
            Layout.fillWidth: true
            backShown: true
            title: page.title
            toolbarObjectName: "editImagesToolbar"
            titleObjectName: "editImagesHeading"
        }

        MessageBox {
            objectName: "editImagesError"
            Layout.margins: Kirigami.Units.largeSpacing
            visible: page.failed
            type: Kirigami.MessageType.Error
            text: i18n("Could not load the images of this item: check the connection to the server and try again")
            actions: [
                Kirigami.Action {
                    text: i18n("Retry")
                    icon.name: "view-refresh"
                    onTriggered: page.imageModel.reload()
                }
            ]
        }
    }

    FileDialog {
        id: imageDialog
        objectName: "editImageFileDialog"
        title: i18n("Choose an Image")
        nameFilters: [i18n("Images (*.jpg *.jpeg *.png *.webp)")]
        onAccepted: page.upload(selectedFile)
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
                onTriggered: page.removeImage()
            }
        ]

        Controls.Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: i18n("Delete this image from the server? It cannot be recovered")
        }
    }

    ListView {
        id: list
        objectName: "editImagesList"

        // On the view, not in its content: the shadow stays under the toolbar while the types scroll
        TopShadow {
            parent: list
            z: 1
            atStart: list.atYBeginning
        }

        model: page.imageModel
        // The buttons are the tab stops: the view only scrolls
        keyNavigationEnabled: false
        // Every type is built at once (there are at most eleven), so the keyboard reaches the buttons below the view
        cacheBuffer: Kirigami.Units.gridUnit * 400
        bottomMargin: Kirigami.Units.largeSpacing
        Accessible.role: Accessible.List
        Accessible.name: page.title

        Kirigami.LoadingPlaceholder {
            FadeInOnShow {}
            anchors.centerIn: parent
            visible: page.loading && list.count === 0
            text: i18n("Loading…")
        }

        header: Item {
            width: list.width
            height: intro.implicitHeight + Kirigami.Units.largeSpacing * 2

            ColumnLayout {
                id: intro
                anchors {
                    left: parent.left
                    right: parent.right
                    top: parent.top
                    margins: Kirigami.Units.largeSpacing
                }
                spacing: Kirigami.Units.smallSpacing

                Controls.Label {
                    objectName: "editImagesItemName"
                    Layout.fillWidth: true
                    visible: text.length > 0
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                    maximumLineCount: 1
                    textFormat: Text.PlainText
                    text: page.itemName
                }
                Controls.Label {
                    Layout.fillWidth: true
                    opacity: 0.7
                    wrapMode: Text.Wrap
                    textFormat: Text.PlainText
                    text: i18n("JPEG, PNG, or WebP, at most 20 MiB")
                }
            }
        }

        delegate: Item {
            id: group

            required property int index
            required property string imageType
            required property bool multiple
            required property var images

            property alias uploadButton: upload
            readonly property string typeLabel: types.label(imageType)
            readonly property real frameHeight: Kirigami.Units.gridUnit * types.frameUnits(imageType)
            readonly property real frameWidth: frameHeight * types.aspect(imageType)

            objectName: "editImagesGroup_" + imageType
            width: ListView.view.width
            height: content.implicitHeight + Kirigami.Units.largeSpacing

            Component.onCompleted: {
                if (index === 0 && page.focusWanted) {
                    page.focusWanted = false;
                    upload.forceActiveFocus(Qt.TabFocusReason);
                }
            }

            ColumnLayout {
                id: content
                anchors {
                    left: parent.left
                    right: parent.right
                    top: parent.top
                    leftMargin: Kirigami.Units.largeSpacing
                    rightMargin: Kirigami.Units.largeSpacing
                }
                spacing: Kirigami.Units.smallSpacing

                Kirigami.Separator {
                    Layout.fillWidth: true
                    Layout.bottomMargin: Kirigami.Units.smallSpacing
                    visible: group.index > 0
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.smallSpacing

                    Kirigami.Heading {
                        Layout.fillWidth: true
                        level: 3
                        elide: Text.ElideRight
                        textFormat: Text.PlainText
                        text: group.typeLabel
                    }
                    ActionButton {
                        id: upload
                        objectName: "editImageUpload_" + group.imageType
                        text: i18n("Upload…")
                        icon.name: "document-open"
                        enabled: !page.writing
                        Accessible.name: i18nc("@action:button %1 is an image type", "Upload %1 Image", group.typeLabel)
                        onClicked: page.choose(group.imageType)
                    }
                    ActionButton {
                        objectName: "editImageSearch_" + group.imageType
                        text: i18n("Search Online…")
                        icon.name: "edit-find"
                        enabled: !page.writing
                        Accessible.name: i18nc("@action:button %1 is an image type", "Search Online for a %1 Image", group.typeLabel)
                        onClicked: page.searchOnline(group.imageType)
                    }
                }

                Controls.Label {
                    objectName: "editImagesNone_" + group.imageType
                    visible: group.images.length === 0 && page.imageModel.status !== EditImagesModel.Loading
                    opacity: 0.7
                    textFormat: Text.PlainText
                    text: i18nc("@info no image of this type", "None")
                }

                Flow {
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.largeSpacing

                    Repeater {
                        model: group.images

                        delegate: Column {
                            id: card

                            required property var modelData
                            required property int index
                            readonly property var image: modelData
                            // A type with several: the place of the image among them
                            readonly property string place: group.multiple ? i18nc("@info the place of an image among those of its type", "%1 of %2", String(card.index + 1), String(group.images.length)) : ""
                            readonly property string described: card.image.imageIndex >= 0 ? i18nc("@info image type and the place of the image among those of its type", "%1 %2", group.typeLabel, card.place) : group.typeLabel

                            objectName: "editImageCard_" + group.imageType + (card.image.imageIndex >= 0 ? "_" + card.image.imageIndex : "")
                            width: group.frameWidth
                            spacing: Kirigami.Units.smallSpacing

                            Rectangle {
                                width: parent.width
                                height: group.frameHeight
                                color: Kirigami.Theme.alternateBackgroundColor
                                radius: Kirigami.Units.cornerRadius
                                border.width: 1
                                border.color: Qt.alpha(Kirigami.Theme.textColor, 0.2)
                                clip: true

                                Kirigami.Icon {
                                    anchors.centerIn: parent
                                    width: Kirigami.Units.iconSizes.large
                                    height: width
                                    visible: thumbnail.status !== Image.Ready
                                    source: "image-x-generic"
                                    opacity: 0.7
                                }
                                Image {
                                    id: thumbnail
                                    objectName: "editImageThumbnail"
                                    anchors.fill: parent
                                    anchors.margins: 1
                                    asynchronous: true
                                    fillMode: Image.PreserveAspectFit
                                    sourceSize.width: width * Screen.devicePixelRatio
                                    // Only a URL the model built with the API client's image URL builder: a raw server string never becomes a source
                                    source: card.image.thumbnailUrl
                                }
                            }

                            Controls.Label {
                                width: parent.width
                                visible: text.length > 0
                                elide: Text.ElideRight
                                maximumLineCount: 1
                                textFormat: Text.PlainText
                                text: card.place
                            }
                            Controls.Label {
                                width: parent.width
                                visible: text.length > 0
                                elide: Text.ElideRight
                                maximumLineCount: 1
                                opacity: 0.7
                                font: Kirigami.Theme.smallFont
                                textFormat: Text.PlainText
                                text: page.dimensions(card.image)
                            }
                            Controls.Label {
                                width: parent.width
                                visible: text.length > 0
                                elide: Text.ElideRight
                                maximumLineCount: 1
                                opacity: 0.7
                                font: Kirigami.Theme.smallFont
                                textFormat: Text.PlainText
                                text: page.fileSize(card.image)
                            }

                            // Disabled while the list reloads after a write, or when that reload failed: the indexes on the cards are the old ones, and a move or a delete by them would hit another image
                            Row {
                                spacing: Kirigami.Units.smallSpacing

                                IconButton {
                                    objectName: "editImageEarlier_" + group.imageType + "_" + card.image.imageIndex
                                    visible: group.multiple
                                    text: i18nc("@action:button", "Move Earlier")
                                    icon.name: Qt.application.layoutDirection === Qt.RightToLeft ? "go-next" : "go-previous"
                                    enabled: !page.writing && !page.loading && !page.failed && card.image.previousIndex >= 0
                                    Accessible.name: i18nc("@action:button %1 is an image type and its place", "Move %1 Earlier", card.described)
                                    onClicked: page.moveImage(group.imageType, card.image.imageIndex, card.image.previousIndex)
                                }
                                IconButton {
                                    objectName: "editImageLater_" + group.imageType + "_" + card.image.imageIndex
                                    visible: group.multiple
                                    text: i18nc("@action:button", "Move Later")
                                    icon.name: Qt.application.layoutDirection === Qt.RightToLeft ? "go-previous" : "go-next"
                                    enabled: !page.writing && !page.loading && !page.failed && card.image.nextIndex >= 0
                                    Accessible.name: i18nc("@action:button %1 is an image type and its place", "Move %1 Later", card.described)
                                    onClicked: page.moveImage(group.imageType, card.image.imageIndex, card.image.nextIndex)
                                }
                                IconButton {
                                    objectName: "editImageDelete_" + group.imageType + (card.image.imageIndex >= 0 ? "_" + card.image.imageIndex : "")
                                    text: i18nc("@action:button", "Delete…")
                                    icon.name: "edit-delete"
                                    enabled: !page.writing && !page.loading && !page.failed
                                    Accessible.name: i18nc("@action:button %1 is an image type and its place", "Delete %1", card.described)
                                    onClicked: page.confirmDelete(group.imageType, card.image.imageIndex)
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
