// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// A backdrop upload adds an image after the others, any other type is replaced, and the details page reads the item again after every write. A page that offers one image type (a person has a Primary image only) shows that image beside its buttons, with no card or type title around it
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
    // The only image types the page offers, in order; empty for every type the item can have
    property var imageTypes: []
    readonly property bool single: imageTypes.length === 1

    // The model is a child of this page and starts loading at once
    readonly property EditImagesModel imageModel: connection.createEditImages(itemId, page, imageTypes)
    readonly property bool loading: imageModel?.status === EditImagesModel.Loading
    readonly property bool failed: imageModel?.status === EditImagesModel.Failed
    // A write of this page is on its way: the buttons wait for the answer, as the images they address are about to change
    property bool writing: false
    property string writeKind: ""
    property string uploadType: ""
    property string deleteType: ""
    property int deleteIndex: -1
    // Set when the keyboard was asked for before the grid had a first card
    property bool focusWanted: false
    // The least width of a card: the grid has as many columns as fit at this width, and the cards share what is left of the row
    readonly property real cardMinWidth: Kirigami.Units.gridUnit * 23
    // The preview of every card has this height, whatever the shape of its image
    readonly property real previewHeight: Kirigami.Units.gridUnit * 8
    // The one image of a page with a single type is shown larger, in a frame of the shape of the usual picture of the type
    readonly property real singlePreviewHeight: Kirigami.Units.gridUnit * 16
    readonly property string uploadText: i18n("Upload…")
    readonly property string searchText: i18n("Search Online…")

    title: i18n("Edit Images")
    // The row of the page carries the way back and the name, as on the Movies and Shows tabs
    globalToolBarStyle: Kirigami.ApplicationHeaderStyle.None
    // Without Kirigami's title row the page names itself to a screen reader
    Accessible.role: Accessible.Pane
    Accessible.name: title

    // The Upload button of the first type takes the keys as soon as the page shows
    function focusPrimary() {
        const first = cards.itemAt(0);
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

    // The size in pixels and the file size on one line
    function details(image) {
        return [dimensions(image), fileSize(image)].filter(part => part.length > 0).join(" · ");
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

    // A button that scrolls itself into view when the keyboard reaches it, and names itself in a tooltip while it shows no label
    component ActionButton: Controls.Button {
        id: actionButton
        onVisualFocusChanged: {
            if (visualFocus)
                Spatial.reveal(actionButton);
        }

        PlainToolTip {
            visible: (actionButton.hovered || actionButton.visualFocus) && actionButton.display === Controls.AbstractButton.IconOnly && actionButton.text.length > 0
            text: actionButton.text
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

    Flickable {
        id: scroller
        objectName: "editImagesList"

        // The page margin at the edges and between the cards
        readonly property real gap: PageMetrics.margin
        readonly property real usableWidth: Math.max(0, width - gap * 2)
        readonly property int columns: Math.max(1, Math.floor((usableWidth + gap) / (page.cardMinWidth + gap)))
        readonly property real cardWidth: Math.floor((usableWidth - gap * (columns - 1)) / columns)

        contentWidth: width
        contentHeight: column.implicitHeight + gap * 2

        // On the view, not in its content: the shadow stays under the toolbar while the cards scroll
        TopShadow {
            parent: scroller
            z: 1
            atStart: scroller.atYBeginning
        }

        ColumnLayout {
            id: column
            x: scroller.gap
            y: scroller.gap
            width: scroller.usableWidth
            spacing: scroller.gap

            // Hidden buttons that measure what the labelled ones need: the real ones lose their labels in a card too narrow for them, and their own width then says nothing
            Item {
                id: measure
                visible: false

                readonly property real labelled: uploadSample.implicitWidth + searchSample.implicitWidth + iconSample.implicitWidth * 3 + Kirigami.Units.smallSpacing * 5
                // The row of a page with one image has the delete button only
                readonly property real labelledSingle: uploadSample.implicitWidth + searchSample.implicitWidth + iconSample.implicitWidth + Kirigami.Units.smallSpacing * 2
                readonly property real rowHeight: Math.max(uploadSample.implicitHeight, searchSample.implicitHeight, iconSample.implicitHeight)

                Controls.Button {
                    id: uploadSample
                    text: page.uploadText
                    icon.name: "document-open"
                }
                Controls.Button {
                    id: searchSample
                    text: page.searchText
                    icon.name: "edit-find"
                }
                Controls.ToolButton {
                    id: iconSample
                    display: Controls.AbstractButton.IconOnly
                    icon.name: "go-next"
                }
            }

            ColumnLayout {
                id: intro
                Layout.fillWidth: true
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

            Kirigami.LoadingPlaceholder {
                FadeInOnShow {}
                Layout.fillWidth: true
                visible: page.loading && cards.count === 0
                text: i18n("Loading…")
            }

            // One card per image type, in the order of the model: left to right, then down
            Grid {
                Layout.alignment: Qt.AlignLeft | Qt.AlignTop
                columns: scroller.columns
                columnSpacing: scroller.gap
                rowSpacing: scroller.gap

                Repeater {
                    id: cards
                    model: page.imageModel

                    delegate: Rectangle {
                        id: group

                        required property int index
                        required property string imageType
                        required property bool multiple
                        required property var images

                        property alias uploadButton: upload
                        readonly property string typeLabel: types.label(imageType)
                        // A page with one image has no card: the image and its buttons sit on the page itself
                        readonly property real padding: page.single ? 0 : Kirigami.Units.largeSpacing * 2
                        // The space between the image and its buttons when they are side by side
                        readonly property real gap: Kirigami.Units.largeSpacing * 2
                        readonly property real previewWidth: page.singlePreviewHeight * types.aspect(imageType)
                        // With one image the buttons stand beside it when they fit there, else below it
                        readonly property bool beside: page.single && width - previewWidth - gap >= measure.labelledSingle
                        // The card shows one image of its type at a time, known by its cache tag: a move changes the place of an image but not its tag, so the card goes on showing the image that was moved. Without a known tag (none chosen yet, or the image was deleted) it shows the first
                        property string shownTag: ""
                        readonly property int shown: {
                            if (shownTag.length > 0) {
                                for (let at = 0; at < images.length; ++at) {
                                    if (images[at].imageTag === shownTag)
                                        return at;
                                }
                            }
                            return 0;
                        }
                        // The place of the shown image among those of a type with several
                        readonly property string place: multiple && images.length > 0 ? i18nc("@info the place of an image among those of its type", "%1 of %2", String(shown + 1), String(images.length)) : ""
                        // The buttons keep their labels while the card has room for them, and the same for every card, as they all have the width
                        readonly property bool compact: page.single ? (beside ? width - previewWidth - gap : width) < measure.labelledSingle : width - padding * 2 < measure.labelled

                        function showPlace(target) {
                            const at = Math.max(0, Math.min(target, images.length - 1));
                            shownTag = images[at]?.imageTag ?? "";
                        }

                        objectName: "editImagesGroup_" + imageType
                        width: page.single ? scroller.usableWidth : scroller.cardWidth
                        height: content.implicitHeight + padding * 2
                        color: page.single ? "transparent" : Kirigami.Theme.alternateBackgroundColor
                        radius: Kirigami.Units.cornerRadius
                        border.width: page.single ? 0 : 1
                        border.color: Qt.alpha(Kirigami.Theme.textColor, 0.2)
                        Accessible.role: Accessible.Grouping
                        Accessible.name: typeLabel

                        Component.onCompleted: {
                            if (index === 0 && page.focusWanted) {
                                page.focusWanted = false;
                                upload.forceActiveFocus(Qt.TabFocusReason);
                            }
                        }

                        ColumnLayout {
                            id: content
                            anchors {
                                fill: parent
                                margins: group.padding
                            }
                            spacing: Kirigami.Units.smallSpacing

                            // The same height with or without the buttons that step through the images, so the cards stay even; a page with one image has no title above it, as the page already says whose image it is
                            RowLayout {
                                visible: !page.single
                                Layout.fillWidth: true
                                Layout.preferredHeight: Math.max(heading.implicitHeight, previous.implicitHeight)
                                spacing: Kirigami.Units.smallSpacing

                                Kirigami.Heading {
                                    id: heading
                                    Layout.fillWidth: true
                                    level: 3
                                    elide: Text.ElideRight
                                    textFormat: Text.PlainText
                                    text: group.typeLabel
                                }
                                IconButton {
                                    id: previous
                                    objectName: "editImagePrevious_" + group.imageType
                                    visible: group.multiple && group.images.length > 1
                                    text: i18nc("@action:button", "Previous Image")
                                    icon.name: Qt.application.layoutDirection === Qt.RightToLeft ? "go-next-view" : "go-previous-view"
                                    Accessible.name: i18nc("@action:button %1 is an image type", "Show the Previous %1 Image", group.typeLabel)
                                    // Round from the first to the last: a button that disabled itself at an end would drop the keyboard focus
                                    onClicked: group.showPlace((group.shown + group.images.length - 1) % group.images.length)
                                }
                                Controls.Label {
                                    objectName: "editImagesPlace_" + group.imageType
                                    visible: text.length > 0
                                    opacity: 0.7
                                    textFormat: Text.PlainText
                                    text: group.place
                                }
                                IconButton {
                                    objectName: "editImageNext_" + group.imageType
                                    visible: previous.visible
                                    text: i18nc("@action:button", "Next Image")
                                    icon.name: Qt.application.layoutDirection === Qt.RightToLeft ? "go-previous-view" : "go-next-view"
                                    Accessible.name: i18nc("@action:button %1 is an image type", "Show the Next %1 Image", group.typeLabel)
                                    onClicked: group.showPlace((group.shown + 1) % group.images.length)
                                }
                            }

                            GridLayout {
                                Layout.fillWidth: true
                                columns: group.beside ? 2 : 1
                                columnSpacing: group.gap
                                rowSpacing: Kirigami.Units.smallSpacing

                                // Every image of the type has its place in the frame and only the shown one is visible; a type with no image says so in the same frame
                                Rectangle {
                                    Layout.fillWidth: !page.single
                                    Layout.preferredWidth: page.single ? group.previewWidth : -1
                                    Layout.preferredHeight: page.single ? page.singlePreviewHeight : page.previewHeight
                                    Layout.alignment: Qt.AlignLeft | Qt.AlignTop
                                    color: page.single ? Kirigami.Theme.alternateBackgroundColor : Kirigami.Theme.backgroundColor
                                    radius: Kirigami.Units.cornerRadius
                                    border.width: 1
                                    border.color: Qt.alpha(Kirigami.Theme.textColor, 0.2)
                                    clip: true

                                    Controls.Label {
                                        objectName: "editImagesNone_" + group.imageType
                                        anchors.centerIn: parent
                                        visible: group.images.length === 0 && page.imageModel.status !== EditImagesModel.Loading
                                        opacity: 0.7
                                        textFormat: Text.PlainText
                                        text: i18nc("@info no image of this type", "None")
                                    }

                                    Repeater {
                                        model: group.images

                                        delegate: Item {
                                            id: slide

                                            required property var modelData
                                            required property int index
                                            readonly property var image: modelData

                                            objectName: "editImageCard_" + group.imageType + (slide.image.imageIndex >= 0 ? "_" + slide.image.imageIndex : "")
                                            anchors.fill: parent
                                            visible: slide.index === group.shown

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
                                                source: slide.image.thumbnailUrl
                                            }
                                        }
                                    }
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Layout.alignment: Qt.AlignLeft | Qt.AlignTop
                                    spacing: Kirigami.Units.smallSpacing

                                    // The line keeps its height when the type has no image
                                    Controls.Label {
                                        id: info
                                        objectName: "editImageInfo_" + group.imageType
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: infoMetrics.height
                                        elide: Text.ElideRight
                                        maximumLineCount: 1
                                        opacity: 0.7
                                        font: Kirigami.Theme.smallFont
                                        textFormat: Text.PlainText
                                        text: group.images.length > 0 ? page.details(group.images[group.shown]) : ""
                                    }
                                    FontMetrics {
                                        id: infoMetrics
                                        font: info.font
                                    }

                                    RowLayout {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: measure.rowHeight
                                        spacing: Kirigami.Units.smallSpacing

                                        ActionButton {
                                            id: upload
                                            objectName: "editImageUpload_" + group.imageType
                                            text: page.uploadText
                                            icon.name: "document-open"
                                            display: group.compact ? Controls.AbstractButton.IconOnly : Controls.AbstractButton.TextBesideIcon
                                            enabled: !page.writing
                                            Accessible.name: i18nc("@action:button %1 is an image type", "Upload %1 Image", group.typeLabel)
                                            onClicked: page.choose(group.imageType)
                                        }
                                        ActionButton {
                                            objectName: "editImageSearch_" + group.imageType
                                            text: page.searchText
                                            icon.name: "edit-find"
                                            display: group.compact ? Controls.AbstractButton.IconOnly : Controls.AbstractButton.TextBesideIcon
                                            enabled: !page.writing
                                            Accessible.name: i18nc("@action:button %1 is an image type", "Search Online for a %1 Image", group.typeLabel)
                                            onClicked: page.searchOnline(group.imageType)
                                        }
                                        // Pushes the buttons of the shown image to the right of the card; with one image they stay next to the others
                                        Item {
                                            visible: !page.single
                                            Layout.fillWidth: true
                                        }

                                        // The buttons of the shown image: disabled while the list reloads after a write, or when that reload failed, as the indexes on the cards are the old ones, and a move or a delete by them would hit another image
                                        Repeater {
                                            model: group.images

                                            delegate: RowLayout {
                                                id: tools

                                                required property var modelData
                                                required property int index
                                                readonly property var image: modelData
                                                // A type with several: the place of the image among them
                                                readonly property string place: group.multiple ? i18nc("@info the place of an image among those of its type", "%1 of %2", String(tools.index + 1), String(group.images.length)) : ""
                                                readonly property string described: tools.image.imageIndex >= 0 ? i18nc("@info image type and the place of the image among those of its type", "%1 %2", group.typeLabel, tools.place) : group.typeLabel

                                                visible: tools.index === group.shown
                                                spacing: Kirigami.Units.smallSpacing

                                                IconButton {
                                                    objectName: "editImageEarlier_" + group.imageType + "_" + tools.image.imageIndex
                                                    visible: group.multiple
                                                    text: i18nc("@action:button", "Move Earlier")
                                                    // An arrow with a shaft moves the image, the chevrons of the heading only step through them
                                                    icon.name: Qt.application.layoutDirection === Qt.RightToLeft ? "arrow-right" : "arrow-left"
                                                    enabled: !page.writing && !page.loading && !page.failed && tools.image.previousIndex >= 0
                                                    Accessible.name: i18nc("@action:button %1 is an image type and its place", "Move %1 Earlier", tools.described)
                                                    onClicked: page.moveImage(group.imageType, tools.image.imageIndex, tools.image.previousIndex)
                                                }
                                                IconButton {
                                                    objectName: "editImageLater_" + group.imageType + "_" + tools.image.imageIndex
                                                    visible: group.multiple
                                                    text: i18nc("@action:button", "Move Later")
                                                    icon.name: Qt.application.layoutDirection === Qt.RightToLeft ? "arrow-left" : "arrow-right"
                                                    enabled: !page.writing && !page.loading && !page.failed && tools.image.nextIndex >= 0
                                                    Accessible.name: i18nc("@action:button %1 is an image type and its place", "Move %1 Later", tools.described)
                                                    onClicked: page.moveImage(group.imageType, tools.image.imageIndex, tools.image.nextIndex)
                                                }
                                                IconButton {
                                                    objectName: "editImageDelete_" + group.imageType + (tools.image.imageIndex >= 0 ? "_" + tools.image.imageIndex : "")
                                                    text: i18nc("@action:button", "Delete…")
                                                    icon.name: "edit-delete"
                                                    enabled: !page.writing && !page.loading && !page.failed
                                                    Accessible.name: i18nc("@action:button %1 is an image type and its place", "Delete %1", tools.described)
                                                    onClicked: page.confirmDelete(group.imageType, tools.image.imageIndex)
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
