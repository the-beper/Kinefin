// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// Everything in the library a person appears in, opened from a cast card: movies, the series they are in the cast of, and one card for each series they only have episodes in, which opens their episodes of it
Kirigami.ScrollablePage {
    id: page
    objectName: "personPage"

    // The darker View background on every page: a page without a list (a plain flickable) would get the lighter Window one, and the sections would not match
    Kirigami.Theme.colorSet: Kirigami.Theme.View
    Kirigami.Theme.inherit: false

    required property ConnectionController connection
    required property string personId
    required property string personName

    readonly property PersonItemsModel model: connection.personItems

    actions: [
        Kirigami.Action {
            objectName: "editPersonImagesAction"
            text: i18n("Edit Images…")
            icon.name: "edit-image"
            visible: page.connection.currentUserIsAdministrator
            // A person has a Primary image only
            onTriggered: page.Window.window.openEditImages(page.personId, page.personName, page, ["Primary"])
        }
    ]

    // The model belongs to the page: the last one to go releases the listing channel
    Component.onDestruction: model.close()

    title: personName
    // The row of the page carries the way back and the name, as on the Movies and Shows tabs
    globalToolBarStyle: Kirigami.ApplicationHeaderStyle.None
    // Without Kirigami's title row the page names itself to a screen reader
    Accessible.role: Accessible.Pane
    Accessible.name: title

    // The grid takes the keys as soon as the page shows
    function focusPrimary() {
        grid.forceActiveFocus(Qt.TabFocusReason);
    }

    // Another person in the same page: the grid starts again at the first item
    function resetView() {
        grid.currentIndex = 0;
        grid.positionViewAtBeginning();
    }

    header: ColumnLayout {
        spacing: 0

        PageToolbar {
            Layout.fillWidth: true
            backShown: true
            title: page.personName
            actions: page.actions
            toolbarObjectName: "personToolbar"
        }

        MessageBox {
            Layout.margins: Kirigami.Units.largeSpacing
            visible: page.model.status === PersonItemsModel.Failed
            type: Kirigami.MessageType.Error
            text: i18n("Could not load this person’s media: check the connection to the server and try again")
            actions: [
                Kirigami.Action {
                    text: i18n("Retry")
                    icon.name: "view-refresh"
                    onTriggered: page.model.retry()
                }
            ]
        }

        MessageBox {
            Layout.margins: Kirigami.Units.largeSpacing
            visible: page.model.pageError && page.model.status !== PersonItemsModel.Failed
            type: Kirigami.MessageType.Warning
            text: i18n("Some items could not be loaded")
            actions: [
                Kirigami.Action {
                    text: i18n("Retry")
                    icon.name: "view-refresh"
                    onTriggered: page.model.retry()
                }
            ]
        }
    }

    GridView {
        id: grid
        // What scrolls under the page row fades into it instead of being cut by its edge
        TopShadow {
            parent: grid
            z: 1
            atStart: grid.atYBeginning
        }
        objectName: "personItemsGrid"
        // The page margin under the bar, as at its sides: the rows scroll up to its line, but open clear of it
        topMargin: PageMetrics.margin

        // Wide tiles of about fourteen grid units, like the Home row
        readonly property real usableWidth: Math.max(0, width - leftMargin)
        readonly property int columns: Math.max(1, Math.floor(usableWidth / (Kirigami.Units.gridUnit * 14)))

        model: page.model
        // The page margin on the left, as the library grid has it: the cards do not touch the edge
        leftMargin: PageMetrics.margin
        cellWidth: Math.floor(usableWidth / columns)
        // A 16:9 image inside the tile margins, then the name and the second line
        cellHeight: Math.round((cellWidth - Kirigami.Units.largeSpacing - Kirigami.Units.smallSpacing * 2) * 9 / 16) + Kirigami.Units.gridUnit * 3.5 + Kirigami.Units.largeSpacing
        activeFocusOnTab: true
        keyNavigationEnabled: true
        currentIndex: 0
        Accessible.role: Accessible.List
        Accessible.name: page.title

        // The tiles are not tab stops: the grid is, and it opens the current item
        function openCurrent() {
            if (grid.currentItem?.enabled)
                grid.currentItem.openTarget();
        }
        Keys.onReturnPressed: openCurrent()
        Keys.onEnterPressed: openCurrent()
        Keys.onSpacePressed: openCurrent()
        Keys.onMenuPressed: if (grid.currentItem?.enabled) grid.currentItem.openMenu(0, grid.currentItem.height)
        Keys.onPressed: event => {
            if (event.modifiers === Qt.ShiftModifier && event.key === Qt.Key_F10 && grid.currentItem?.enabled) {
                grid.currentItem.openMenu(0, grid.currentItem.height);
                event.accepted = true;
            }
        }

        Kirigami.LoadingPlaceholder {
            FadeInOnShow {}
            anchors.centerIn: parent
            visible: page.model.status === PersonItemsModel.Loading && grid.count === 0
            text: i18n("Loading…")
        }

        Kirigami.PlaceholderMessage {
            FadeInOnShow {}
            anchors.centerIn: parent
            width: parent.width - Kirigami.Units.gridUnit * 4
            visible: page.model.status === PersonItemsModel.Ready && page.model.totalCount === 0
            icon.name: "user-identity"
            text: i18n("No media with this person")
            explanation: i18n("Movies, shows, and episodes featuring this person show up here")
        }

        delegate: MediaCard {
            // A series the person only has episodes in: how many (0 for any other card)
            required property int guestEpisodes

            width: grid.cellWidth - Kirigami.Units.largeSpacing
            height: grid.cellHeight - Kirigami.Units.largeSpacing
            // The grid is the one tab stop, the arrow keys move inside it
            focusPolicy: Qt.NoFocus
            landscape: true
            // A guest-series card stands for the person's episodes: a menu would act on the whole series, without its favorite or played state
            hasMenu: guestEpisodes === 0
            highlighted: GridView.isCurrentItem && grid.activeFocus
            onClicked: openTarget()
            subtitle: guestEpisodes > 0 ? i18np("%1 episode", "%1 episodes", guestEpisodes) : (year > 0 ? String(year) : "")

            function openTarget() {
                if (guestEpisodes > 0)
                    page.Window.window.openPersonSeries(itemId, name, page.personName);
                else
                    page.Window.window.openItem(itemId, page);
            }
        }
    }
}
