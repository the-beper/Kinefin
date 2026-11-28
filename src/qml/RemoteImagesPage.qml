// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The images the metadata providers offer for one item (administrators only), above its Edit Images page: pick the image type (not when the Edit Images page offers one type, as for a person), a provider and whether to include every language, and choose one of the pictures, a page at a time, to have the server download it as that type of image of the item. The page goes back to Edit Images once the server has saved the image
Kirigami.ScrollablePage {
    id: page
    objectName: "remoteImagesPage"

    // The darker View background on every page: a page without a list (a plain flickable) would get the lighter Window one, and the sections would not match
    Kirigami.Theme.colorSet: Kirigami.Theme.View
    Kirigami.Theme.inherit: false

    required property ConnectionController connection
    required property string itemId
    // The type to look for first
    required property string imageType
    // The only image types the page offers, as the Edit Images page it opens above does; empty for every type the providers offer. With one type there is nothing to pick
    property var imageTypes: []
    readonly property bool single: imageTypes.length === 1
    // The item's name as the caller knew it: server text, shown as plain text only
    property string itemName: ""
    // Choosing an image would take the place of one the item has: the choice asks first, since the old image cannot be recovered
    property bool replaces: false
    // The tile waiting for the answer of that question
    property int pendingRow: -1

    // The model is a child of this page and starts loading at once
    readonly property RemoteImagesModel offers: connection.createRemoteImages(itemId, imageType, page)
    readonly property bool downloading: offers?.downloadState === RemoteImagesModel.Downloading
    // The choices of the two boxes: the types and the providers the model offers (provider names are server text)
    readonly property var typeChoices: (offers?.imageTypes ?? []).filter(type => imageTypes.length === 0 || imageTypes.includes(type)).map(type => ({ value: type, text: types.label(type) }))
    readonly property var providerChoices: [{ value: "", text: i18n("All providers") }].concat((offers?.providerNames ?? []).map(name => ({ value: name, text: name })))

    title: i18n("Search Online")
    // The row of the page carries the way back and the name, as on the Movies and Shows tabs
    globalToolBarStyle: Kirigami.ApplicationHeaderStyle.None
    // Without Kirigami's title row the page names itself to a screen reader
    Accessible.role: Accessible.Pane
    Accessible.name: title

    // The grid takes the keys as soon as the page shows
    function focusPrimary() {
        grid.forceActiveFocus(Qt.TabFocusReason);
    }

    // What the main menu's Refresh calls while this page is in front
    function refreshPage() {
        if (offers && offers.status !== RemoteImagesModel.Loading)
            offers.reload();
    }

    function indexOfValue(choices, value) {
        const found = choices.findIndex(choice => choice.value === value);
        return found < 0 ? 0 : found;
    }

    // The picture the user chose: the server downloads it as an image of the type shown
    function pick(row) {
        if (row < 0 || row >= grid.count)
            return;
        if (replaces) {
            pendingRow = row;
            replaceDialog.open();
            return;
        }
        download(row);
    }

    function download(row) {
        offers.dismissDownloadError();
        offers.download(row);
    }

    function infoLine(imageWidth, imageHeight, language) {
        const parts = [];
        if (imageWidth > 0 && imageHeight > 0)
            parts.push(i18nc("@info image size in pixels, width then height", "%1 × %2", String(imageWidth), String(imageHeight)));
        if (language.length > 0)
            parts.push(language);
        return parts.join(" · ");
    }

    // What the provider's users said of the picture: a score and its votes, or a count of likes; empty when nothing
    function ratingText(rating, voteCount, ratingType) {
        if (rating <= 0)
            return "";
        if (ratingType === "Likes")
            return i18np("%1 like", "%1 likes", Math.round(rating));
        const score = Number(rating).toLocaleString(Qt.locale(), "f", 1);
        return voteCount > 0 ? i18np("%2 (%1 vote)", "%2 (%1 votes)", voteCount, score) : score;
    }

    function downloadErrorText() {
        if (offers.downloadStatus === 403)
            return i18n("The server did not allow changing the images of this item");
        return i18n("The image could not be downloaded: try another one, or check the connection to the server");
    }

    Connections {
        target: page.offers
        function onDownloaded() {
            page.Window.window?.showPassiveNotification(i18n("Image updated"));
            // Back to the Edit Images page, which shows the new image
            if (page.isCurrentPage)
                page.Window.window?.pageStack.goBack();
        }
        // A new list starts at its first picture
        function onStatusChanged() {
            if (page.offers.status === RemoteImagesModel.Loading) {
                grid.positionViewAtBeginning();
                grid.currentIndex = 0;
            }
        }
    }

    ImageTypes {
        id: types
    }

    Kirigami.PromptDialog {
        id: replaceDialog
        objectName: "replaceImageDialog"

        title: i18n("Replace Image")
        standardButtons: Kirigami.Dialog.Cancel
        customFooterActions: [
            Kirigami.Action {
                objectName: "confirmReplaceImage"
                text: i18n("Replace")
                icon.name: "dialog-ok-apply"
                onTriggered: {
                    replaceDialog.close();
                    page.download(page.pendingRow);
                }
            }
        ]
        onClosed: page.focusPrimary()

        Controls.Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: page.single ? i18n("Replace the current image on the server? It cannot be recovered") : i18n("Replace the current image of this type on the server? It cannot be recovered")
        }
    }

    header: ColumnLayout {
        spacing: 0

        PageToolbar {
            Layout.fillWidth: true
            backShown: true
            title: page.title
            toolbarObjectName: "remoteImagesToolbar"
            titleObjectName: "remoteImagesHeading"
        }

        Controls.Label {
            objectName: "remoteImagesHint"
            Layout.fillWidth: true
            // Room under the bar, as the lists of the other pages leave
            Layout.topMargin: PageMetrics.margin
            Layout.leftMargin: PageMetrics.margin
            Layout.rightMargin: Kirigami.Units.largeSpacing
            wrapMode: Text.Wrap
            opacity: 0.7
            textFormat: Text.PlainText
            text: i18n("Choose an image to use it for this item")
        }

        Controls.Label {
            objectName: "remoteImagesItemName"
            Layout.fillWidth: true
            Layout.margins: Kirigami.Units.largeSpacing
            Layout.bottomMargin: 0
            visible: text.length > 0
            font.weight: Font.DemiBold
            elide: Text.ElideRight
            maximumLineCount: 1
            textFormat: Text.PlainText
            text: page.itemName
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: Kirigami.Units.largeSpacing
            spacing: Kirigami.Units.largeSpacing
            enabled: !page.downloading

            Controls.Label {
                visible: !page.single
                textFormat: Text.PlainText
                text: i18nc("@label:listbox", "Image type:")
            }
            ChoiceBox {
                id: typeBox
                objectName: "remoteImagesTypeBox"
                visible: !page.single
                Layout.fillWidth: true
                Layout.preferredWidth: Kirigami.Units.gridUnit * 8
                Layout.maximumWidth: Kirigami.Units.gridUnit * 14
                model: page.typeChoices
                textRole: "text"
                currentIndex: page.indexOfValue(page.typeChoices, page.offers.imageType)
                Accessible.name: i18nc("@label:listbox", "Image type")
                onActivated: index => {
                    page.offers.dismissDownloadError();
                    page.offers.imageType = page.typeChoices[index].value;
                    currentIndex = Qt.binding(() => page.indexOfValue(page.typeChoices, page.offers.imageType));
                }
            }
            Controls.Label {
                textFormat: Text.PlainText
                text: i18nc("@label:listbox", "Provider:")
            }
            ChoiceBox {
                id: providerBox
                objectName: "remoteImagesProviderBox"
                Layout.fillWidth: true
                Layout.preferredWidth: Kirigami.Units.gridUnit * 8
                Layout.maximumWidth: Kirigami.Units.gridUnit * 14
                model: page.providerChoices
                textRole: "text"
                currentIndex: page.indexOfValue(page.providerChoices, page.offers.provider)
                Accessible.name: i18nc("@label:listbox", "Provider")
                onActivated: index => {
                    page.offers.dismissDownloadError();
                    page.offers.provider = page.providerChoices[index].value;
                    currentIndex = Qt.binding(() => page.indexOfValue(page.providerChoices, page.offers.provider));
                }
            }
        }

        Controls.CheckBox {
            objectName: "remoteImagesAllLanguages"
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            enabled: !page.downloading
            text: i18n("Include all languages")
            checked: page.offers.allLanguages
            // A click, not toggled: that also fires when the binding above sets checked
            onClicked: {
                page.offers.dismissDownloadError();
                page.offers.allLanguages = checked;
                checked = Qt.binding(() => page.offers.allLanguages);
            }
        }

        MessageBox {
            objectName: "remoteImagesError"
            Layout.margins: Kirigami.Units.largeSpacing
            visible: page.offers.status === RemoteImagesModel.Failed
            type: Kirigami.MessageType.Error
            text: i18n("Could not load the images from the providers: check the connection to the server and try again")
            actions: [
                Kirigami.Action {
                    text: i18n("Retry")
                    icon.name: "view-refresh"
                    onTriggered: page.offers.reload()
                }
            ]
        }

        MessageBox {
            objectName: "remoteImagesDownloadError"
            Layout.margins: Kirigami.Units.largeSpacing
            visible: page.offers.downloadState === RemoteImagesModel.DownloadFailed
            type: Kirigami.MessageType.Error
            text: page.downloadErrorText()
        }
    }

    footer: Item {
        id: pager

        readonly property bool shown: page.offers.hasPrevious || page.offers.hasNext

        objectName: "remoteImagesPager"
        visible: shown
        implicitHeight: shown ? paging.implicitHeight + Kirigami.Units.largeSpacing * 2 : 0

        Kirigami.Separator {
            anchors {
                left: parent.left
                right: parent.right
                top: parent.top
            }
        }

        RowLayout {
            id: paging
            anchors {
                left: parent.left
                right: parent.right
                verticalCenter: parent.verticalCenter
                margins: Kirigami.Units.largeSpacing
            }
            spacing: Kirigami.Units.largeSpacing

            Controls.Button {
                objectName: "remoteImagesPrevious"
                text: i18nc("@action:button previous page of images", "Previous")
                icon.name: Qt.application.layoutDirection === Qt.RightToLeft ? "go-next" : "go-previous"
                enabled: page.offers.hasPrevious && page.offers.status !== RemoteImagesModel.Loading && !page.downloading
                onClicked: page.offers.previousPage()
            }
            Controls.Label {
                objectName: "remoteImagesRange"
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
                textFormat: Text.PlainText
                text: i18nc("@info the range of images shown and how many there are", "%1 to %2 of %3", String(page.offers.firstShown), String(page.offers.lastShown), String(page.offers.total))
            }
            Controls.Button {
                objectName: "remoteImagesNext"
                text: i18nc("@action:button next page of images", "Next")
                icon.name: Qt.application.layoutDirection === Qt.RightToLeft ? "go-previous" : "go-next"
                enabled: page.offers.hasNext && page.offers.status !== RemoteImagesModel.Loading && !page.downloading
                onClicked: page.offers.nextPage()
            }
        }
    }

    GridView {
        id: grid
        objectName: "remoteImagesGrid"

        // On the view, not in its content: the shadow stays under the bar while the pictures scroll
        TopShadow {
            parent: grid
            z: 1
            atStart: grid.atYBeginning
        }

        readonly property real gutter: PageMetrics.margin
        readonly property real aspect: types.aspect(page.offers.imageType)
        readonly property real usableWidth: Math.max(0, width - leftMargin)
        readonly property int columns: Math.max(1, Math.floor(usableWidth / (Kirigami.Units.gridUnit * types.tileUnits(page.offers.imageType))))

        model: page.offers
        topMargin: PageMetrics.margin
        leftMargin: PageMetrics.margin
        cellWidth: Math.floor(usableWidth / columns)
        // The picture inside the tile margins, then the provider, the size and the rating
        cellHeight: Math.round((cellWidth - gutter - Kirigami.Units.smallSpacing * 2) / aspect) + Kirigami.Units.gridUnit * 4.5 + gutter
        activeFocusOnTab: true
        keyNavigationEnabled: true
        currentIndex: 0
        Accessible.role: Accessible.List
        Accessible.name: page.title

        // The tiles are not tab stops: the grid is, and it downloads the current one
        function pickCurrent() {
            if (grid.currentItem)
                page.pick(grid.currentIndex);
        }
        Keys.onReturnPressed: pickCurrent()
        Keys.onEnterPressed: pickCurrent()
        Keys.onSpacePressed: pickCurrent()

        // The placeholders sit on the view, not in its content
        Kirigami.LoadingPlaceholder {
            FadeInOnShow {}
            parent: grid
            anchors.centerIn: parent
            visible: page.offers.status === RemoteImagesModel.Loading
            text: i18n("Loading…")
        }

        Kirigami.PlaceholderMessage {
            FadeInOnShow {}
            objectName: "remoteImagesEmpty"
            parent: grid
            anchors.centerIn: parent
            width: parent.width - Kirigami.Units.gridUnit * 4
            visible: page.offers.status === RemoteImagesModel.Ready && grid.count === 0
            icon.name: "edit-find"
            text: i18n("No images found")
            explanation: i18n("Try another provider or image type, or include all languages")
        }

        // The server fetches the picture from the provider while the request waits: the grid holds still
        Rectangle {
            objectName: "remoteImagesDownloading"
            parent: grid
            z: 2
            anchors.fill: parent
            visible: page.downloading
            color: Qt.alpha(Kirigami.Theme.backgroundColor, 0.8)

            MouseArea {
                anchors.fill: parent
                hoverEnabled: true
                acceptedButtons: Qt.AllButtons
                onWheel: wheel => wheel.accepted = true
            }
            Kirigami.LoadingPlaceholder {
                objectName: "remoteImagesDownloadingText"
                FadeInOnShow {}
                anchors.centerIn: parent
                text: i18n("Downloading…")
            }
        }

        delegate: Controls.ItemDelegate {
            id: tile

            required property int index
            required property string provider
            required property url thumbnailUrl
            required property int imageWidth
            required property int imageHeight
            required property string language
            required property real rating
            required property int voteCount
            required property string ratingType

            readonly property string info: page.infoLine(imageWidth, imageHeight, language)
            readonly property string ratingLine: page.ratingText(rating, voteCount, ratingType)

            objectName: "remoteImage_" + index
            width: grid.cellWidth - grid.gutter
            height: grid.cellHeight - grid.gutter
            // The grid is the one tab stop, the arrow keys move inside it
            focusPolicy: Qt.NoFocus
            hoverEnabled: true
            padding: Kirigami.Units.smallSpacing
            // The focus ring shows only once the keys are in use, as on the cards of the other pages
            highlighted: GridView.isCurrentItem && grid.activeFocus && (page.Window.window?.keyboardDriven ?? true)
            Accessible.name: provider.length > 0 ? provider + (info.length > 0 ? ", " + info : "") : (info.length > 0 ? info : i18nc("@info accessible name of an image with no details, %1 is its place in the list", "Image %1", String(index + 1)))
            Accessible.description: ratingLine

            // A click makes this tile the current one and gives its grid the focus, so the focus comes back to this tile after Back
            onPressed: {
                grid.currentIndex = tile.index;
                grid.forceActiveFocus(Qt.MouseFocusReason);
            }
            onClicked: page.pick(tile.index)

            // The default delegate background paints the focus ring as a filled highlight; here the ring is only a border
            background: Rectangle {
                color: tile.hovered ? Qt.alpha(Kirigami.Theme.highlightColor, 0.15) : "transparent"
                radius: Kirigami.Units.cornerRadius
                border.width: tile.highlighted ? 2 : 0
                border.color: Kirigami.Theme.highlightColor
            }

            contentItem: ColumnLayout {
                spacing: 0

                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.bottomMargin: Kirigami.Units.smallSpacing
                    color: Kirigami.Theme.alternateBackgroundColor
                    radius: Kirigami.Units.cornerRadius
                    clip: true

                    Kirigami.Icon {
                        anchors.centerIn: parent
                        width: Kirigami.Units.iconSizes.huge
                        height: width
                        visible: preview.status !== Image.Ready
                        source: "image-x-generic"
                        opacity: 0.7
                    }
                    Image {
                        id: preview
                        anchors.fill: parent
                        asynchronous: true
                        fillMode: Image.PreserveAspectFit
                        sourceSize.width: width * Screen.devicePixelRatio
                        // The provider's preview, only when the model kept it as an https URL on a provider's domain; the image fetcher admits nothing else
                        source: tile.thumbnailUrl
                    }
                }

                Controls.Label {
                    Layout.fillWidth: true
                    visible: text.length > 0
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                    maximumLineCount: 1
                    textFormat: Text.PlainText
                    text: tile.provider
                }
                Controls.Label {
                    Layout.fillWidth: true
                    visible: text.length > 0
                    elide: Text.ElideRight
                    maximumLineCount: 1
                    opacity: 0.7
                    font: Kirigami.Theme.smallFont
                    textFormat: Text.PlainText
                    text: tile.info
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: tile.ratingLine.length > 0
                    spacing: Kirigami.Units.smallSpacing

                    Kirigami.Icon {
                        Layout.preferredWidth: Kirigami.Units.iconSizes.small
                        Layout.preferredHeight: Kirigami.Units.iconSizes.small
                        visible: tile.ratingType !== "Likes"
                        source: "starred-symbolic"
                    }
                    Controls.Label {
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        maximumLineCount: 1
                        opacity: 0.7
                        font: Kirigami.Theme.smallFont
                        textFormat: Text.PlainText
                        text: tile.ratingLine
                    }
                }
            }
        }
    }
}
