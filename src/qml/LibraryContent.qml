// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

// The items of a LibraryItemsModel as a grid of cards or a list of rows, by its `viewMode`: one tab stop for the view (arrows to move, Return and Space to open, the Menu key and Shift+F10 for the card menu), its scroll position kept for the page's Back, the scroll back at the first row when the order, filters or view change, and the placeholders of the empty, loading and filtered-empty states
Item {
    id: content
    objectName: "libraryContent"

    required property LibraryItemsModel listModel
    // The page the items open from: the pages above it give way to the details
    required property Item ownerPage
    required property string accessibleName
    // What the placeholder says when the list has no item and nothing narrows it
    property string emptyText: ""
    // Whether the page has filters the filtered-empty placeholder can clear
    property bool canClear: false

    // Set once the content is built: a change of `listModel` before that is only its first binding (an onChanged handler fires for it), which must not scroll away from a position the page restores
    property bool ready: false
    Component.onCompleted: {
        ready = true;
        viewKept = viewKept || firstPageIn;
    }

    signal clearRequested

    readonly property bool listMode: listModel.viewMode === "list"
    readonly property var view: loader.item
    readonly property int count: view ? view.count : 0
    readonly property bool empty: listModel.status === LibraryItemsModel.Ready && listModel.totalCount === 0

    // The image area of a card by image type: its height over its width, and how wide a tile is in grid units
    readonly property var aspects: ({ Primary: 3 / 2, Banner: 0.185, Disc: 1, Logo: 9 / 16, Thumb: 9 / 16 })
    readonly property var tileUnits: ({ Primary: 10, Banner: 22, Disc: 10, Logo: 14, Thumb: 14 })
    readonly property real aspect: aspects[listModel.imageType] ?? 3 / 2
    // The text under the image: the name row and the year line, or only a margin without the card layout
    readonly property real footerHeight: listModel.cardLayout ? Kirigami.Units.gridUnit * (listModel.showYear ? 3.5 : 2.25) : Kirigami.Units.gridUnit * 0.75

    // The scroll position and the current item, for the tab to come back as it was left
    function viewPosition() {
        // A position still waiting for the view is the one to keep
        if (!view && pendingPosition)
            return pendingPosition;
        return { currentIndex: view ? view.currentIndex : 0, contentY: view ? view.contentY : 0 };
    }

    // Applied once the view has rows: the tab may still be loading its first page
    property var pendingPosition: null
    function restoreViewPosition(position) {
        pendingPosition = position;
        applyPendingPosition();
    }
    function applyPendingPosition() {
        if (!pendingPosition || !view || view.count === 0)
            return;
        const position = pendingPosition;
        pendingPosition = null;
        view.currentIndex = Math.min(position.currentIndex, view.count - 1);
        // The page may reset the scroll while it is created: the position goes in after that
        Qt.callLater(() => {
            if (content.view)
                content.view.contentY = Math.min(position.contentY, Math.max(0, content.view.contentHeight - content.view.height));
        });
    }

    function focusView() {
        if (view) {
            view.forceActiveFocus(Qt.TabFocusReason);
            return;
        }
        focusPending = true;
        focusBefore = Window.activeFocusItem;
    }

    // Asked before the view exists: the view takes the keys when it comes, unless the user took the focus out of the page meanwhile (Kirigami moves it inside the page by itself)
    property bool focusPending: false
    property var focusBefore: null

    function focusLeftPage() {
        const focused = Window.activeFocusItem;
        if (!focused || focused === focusBefore)
            return false;
        for (let item = focused; item; item = item.parent) {
            if (item === ownerPage)
                return false;
        }
        return true;
    }

    onViewChanged: {
        topFling.stop();
        if (!view)
            return;
        applyPendingPosition();
        if (focusPending && !focusLeftPage())
            view.forceActiveFocus(Qt.TabFocusReason);
        focusPending = false;
        focusBefore = null;
    }

    // Nothing more is coming to show at once: the view is there, or the first page is still out (the spinner shows)
    readonly property bool settled: view !== null || !firstPageIn

    // Whether the view shows its first row: Return to Top has nothing to do there
    readonly property bool atStart: !view || view.atYBeginning

    // A new order, filter or view starts from the first result
    function scrollToStart() {
        pendingPosition = null;
        topFling.stop();
        if (!view)
            return;
        view.currentIndex = 0;
        view.positionViewAtBeginning();
        // The first row at the top edge leaves the top margin hidden: the start is above it
        view.contentY = view.originY - view.topMargin;
    }

    Connections {
        target: content.listModel
        function onSortByChanged() {
            Qt.callLater(content.scrollToStart);
        }
        function onDescendingChanged() {
            Qt.callLater(content.scrollToStart);
        }
        function onFiltersChanged() {
            Qt.callLater(content.scrollToStart);
        }
        function onSearchTermChanged() {
            Qt.callLater(content.scrollToStart);
        }
    }

    Connections {
        target: content.view
        function onCountChanged() {
            content.applyPendingPosition();
        }
        // The user takes the view over
        function onMovementStarted() {
            topFling.stop();
        }
    }

    // Return to Top: a fling that slows down at the first row; a long way is cut short first, since every row passed would be built on the way
    function returnToTop() {
        if (!view)
            return;
        view.cancelFlick();
        const top = view.originY - view.topMargin;
        if (view.contentY - top > view.height * 2)
            view.contentY = top + view.height * 2;
        topFling.from = view.contentY;
        topFling.to = top;
        topFling.restart();
    }

    NumberAnimation {
        id: topFling
        target: content.view
        property: "contentY"
        duration: Kirigami.Units.veryLongDuration
        easing.type: Easing.OutCubic
        onFinished: {
            if (content.view)
                content.view.currentIndex = 0;
        }
    }

    // The keys of the view: Return, Enter and Space open the current item, Menu and Shift+F10 open its menu
    function openCurrent() {
        if (view?.currentItem?.enabled)
            ownerPage.Window.window.openItem(view.currentItem.itemId, ownerPage);
    }
    function openCurrentMenu() {
        if (view?.currentItem?.enabled)
            view.currentItem.openMenu(0, view.currentItem.height);
    }
    function handleKey(event) {
        if (event.modifiers === Qt.ShiftModifier && event.key === Qt.Key_F10) {
            openCurrentMenu();
            event.accepted = true;
        }
    }

    // The view comes once the first page of items is in, and stays: built asynchronously, its visible cards arrive a few per frame (a view created in an asynchronous incubation creates its delegates the same way), where items reaching a view that already exists would build them all in one frame
    readonly property bool firstPageIn: listModel.status !== LibraryItemsModel.Loading || listModel.totalCount > 0
    property bool viewKept: false
    onFirstPageInChanged: {
        if (firstPageIn)
            viewKept = true;
    }

    Loader {
        id: loader

        anchors.fill: parent
        asynchronous: true
        active: content.viewKept
        sourceComponent: content.listMode ? listComponent : gridComponent
        // The grid or the list that takes the other's place fades in
        onLoaded: viewFade.restart()
    }
    // On the GUI thread: a render-thread animator can leave a stale frame when its item is hidden mid-fade
    NumberAnimation {
        id: viewFade
        property: "opacity"
        target: loader
        from: 0
        to: 1
        duration: Kirigami.Units.longDuration
        easing.type: Easing.OutCubic
    }

    Component {
        id: gridComponent

        GridView {
            id: grid
            objectName: "libraryGrid"

            // A narrow window must be allowed a single legible poster instead of squeezing two cards below their minimum useful width
            readonly property int columns: Math.max(1, Math.floor(usableWidth / (Kirigami.Units.gridUnit * (content.tileUnits[content.listModel.imageType] ?? 10))))
            // The gap between two cards and the margin around them; the width the page margin and the vertical scroll bar take is not shared out to the cells, so no card sits under the bar
            readonly property real gutter: Kirigami.Units.largeSpacing
            readonly property real usableWidth: Math.max(0, width - leftMargin - vbar.implicitWidth)

            clip: true
            leftMargin: gutter
            topMargin: gutter
            boundsBehavior: Flickable.StopAtBounds
            model: content.listModel
            cellWidth: Math.floor(usableWidth / columns)
            // The image area inside the tile margins, then the text under it, then the gutter
            cellHeight: Math.round((cellWidth - gutter - Kirigami.Units.smallSpacing * 2) * content.aspect) + content.footerHeight + gutter
            activeFocusOnTab: true
            keyNavigationEnabled: true
            currentIndex: 0
            Accessible.role: Accessible.List
            Accessible.name: content.accessibleName

            Controls.ScrollBar.vertical: Controls.ScrollBar {
                id: vbar
                objectName: "libraryGridScrollBar"
            }

            Keys.onReturnPressed: content.openCurrent()
            Keys.onEnterPressed: content.openCurrent()
            Keys.onSpacePressed: content.openCurrent()
            Keys.onMenuPressed: content.openCurrentMenu()
            Keys.onPressed: event => content.handleKey(event)

            delegate: MediaCard {
                required property string seriesName
                required property int seasonNumber
                required property int episodeNumber

                width: grid.cellWidth - grid.gutter
                height: grid.cellHeight - grid.gutter
                // The grid is the one tab stop, the arrow keys move inside it
                focusPolicy: Qt.NoFocus
                highlighted: GridView.isCurrentItem && grid.activeFocus
                imageAspect: content.aspect
                fitImage: content.listModel.imageType === "Disc" || content.listModel.imageType === "Logo"
                showTitle: content.listModel.showTitle
                showYear: content.listModel.showYear
                cardLayout: content.listModel.cardLayout
                // An episode is named by its title: the series and the numbers go under it
                subtitle: {
                    if (type !== "Episode" || episodeNumber < 0)
                        return "";
                    const label = seasonNumber >= 0 ? i18nc("season and episode number", "S%1:E%2", seasonNumber, episodeNumber) : i18nc("episode number", "E%1", episodeNumber);
                    return seriesName.length > 0 ? i18nc("series name and episode label", "%1 · %2", seriesName, label) : label;
                }
                spokenSubtitle: {
                    if (type !== "Episode" || episodeNumber < 0)
                        return "";
                    const label = seasonNumber >= 0 ? i18nc("season and episode number spoken", "Season %1, Episode %2", seasonNumber, episodeNumber) : i18nc("episode number spoken", "Episode %1", episodeNumber);
                    return seriesName.length > 0 ? i18nc("series name and episode label", "%1 · %2", seriesName, label) : label;
                }
                onClicked: content.ownerPage.Window.window.openItem(itemId, content.ownerPage)
            }
        }
    }

    Component {
        id: listComponent

        ListView {
            id: list
            objectName: "libraryList"

            clip: true
            boundsBehavior: Flickable.StopAtBounds
            model: content.listModel
            activeFocusOnTab: true
            keyNavigationEnabled: true
            currentIndex: 0
            Accessible.role: Accessible.List
            Accessible.name: content.accessibleName

            Controls.ScrollBar.vertical: Controls.ScrollBar {}

            Keys.onReturnPressed: content.openCurrent()
            Keys.onEnterPressed: content.openCurrent()
            Keys.onSpacePressed: content.openCurrent()
            Keys.onMenuPressed: content.openCurrentMenu()
            Keys.onPressed: event => content.handleKey(event)

            delegate: ItemListRow {
                width: ListView.view ? ListView.view.width : implicitWidth
                // The list is the one tab stop, the arrow keys move inside it
                focusPolicy: Qt.NoFocus
                highlighted: ListView.isCurrentItem && list.activeFocus
                onClicked: content.ownerPage.Window.window.openItem(itemId, content.ownerPage)
            }
        }
    }

    TopFade {
        objectName: "libraryTopFade"
        atStart: content.atStart
    }

    Kirigami.LoadingPlaceholder {
        FadeInOnShow {}
        objectName: "libraryLoading"
        anchors.centerIn: parent
        // Until the view shows the items that are already in
        visible: content.count === 0 && (content.listModel.status === LibraryItemsModel.Loading || (content.listModel.totalCount > 0 && !content.view))
        text: content.listModel.resolvingFacets ? i18n("Applying the saved filters…") : i18n("Loading…")
    }

    // Nothing in the library, and nothing narrows it
    Kirigami.PlaceholderMessage {
        FadeInOnShow {}
        objectName: "libraryEmpty"
        anchors.centerIn: parent
        width: parent.width - Kirigami.Units.gridUnit * 4
        visible: content.empty && !content.listModel.filtered
        icon.name: "folder-video"
        text: i18n("No items")
        explanation: content.emptyText
    }

    // The filters or the search leave no item: the library is not empty
    Kirigami.PlaceholderMessage {
        FadeInOnShow {}
        objectName: "libraryFilteredEmpty"
        anchors.centerIn: parent
        width: parent.width - Kirigami.Units.gridUnit * 4
        visible: content.empty && content.listModel.filtered
        icon.name: "view-filter"
        text: i18n("No items")
        explanation: i18n("No item matches the search or the selected filters")
        helpfulAction: Kirigami.Action {
            objectName: "libraryClearFiltersAction"
            // The placeholder shows its button for an enabled action
            enabled: content.canClear
            text: i18n("Clear Filters")
            icon.name: "edit-clear-all"
            onTriggered: content.clearRequested()
        }
    }
}
