// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The whole Continue Watching list, the "Show All" page of the Home row
Kirigami.ScrollablePage {
    id: page
    objectName: "resumeItemsPage"

    required property ConnectionController connection

    readonly property ResumeItemsModel model: connection.resumeItems
    // The window shell counts the pages: the list closes with the last one
    required property var shell

    Component.onDestruction: shell?.resumePageGone?.()

    title: i18n("Continue Watching")
    // The row of the page carries the way back and the name, as on the Movies and Shows tabs
    globalToolBarStyle: Kirigami.ApplicationHeaderStyle.None
    // Without Kirigami's title row the page names itself to a screen reader
    Accessible.role: Accessible.Pane
    Accessible.name: title

    // The grid takes the keys as soon as the page shows
    function focusPrimary() {
        grid.forceActiveFocus(Qt.TabFocusReason);
    }

    header: ColumnLayout {
        spacing: 0

        PageToolbar {
            Layout.fillWidth: true
            backShown: true
            title: page.title
        }

        MessageBox {
            Layout.margins: Kirigami.Units.largeSpacing
            visible: page.model.status === ResumeItemsModel.Failed
            type: Kirigami.MessageType.Error
            text: i18n("Could not load this list: check the connection to the server and try again")
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
            visible: page.model.pageError && page.model.status !== ResumeItemsModel.Failed
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
        TopFade {
            parent: grid
            z: 1
            atStart: grid.atYBeginning
        }
        objectName: "resumeItemsGrid"

        // Wide tiles of about fourteen grid units, like the Home row
        readonly property int columns: Math.max(1, Math.floor(width / (Kirigami.Units.gridUnit * 14)))

        model: page.model
        cellWidth: Math.floor(width / columns)
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
                page.Window.window.openItem(grid.currentItem.itemId, page);
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
            visible: page.model.status === ResumeItemsModel.Loading && grid.count === 0
            text: i18n("Loading…")
        }

        Kirigami.PlaceholderMessage {
            FadeInOnShow {}
            anchors.centerIn: parent
            width: parent.width - Kirigami.Units.gridUnit * 4
            visible: page.model.status === ResumeItemsModel.Ready && page.model.totalCount === 0
            icon.name: "media-playback-start"
            text: i18n("Nothing to continue watching")
            explanation: i18n("Items you have started watching show up here")
        }

        delegate: MediaCard {
            required property int seasonNumber
            required property int episodeNumber
            required property string episodeTitle

            width: grid.cellWidth - Kirigami.Units.largeSpacing
            height: grid.cellHeight - Kirigami.Units.largeSpacing
            // The grid is the one tab stop, the arrow keys move inside it
            focusPolicy: Qt.NoFocus
            landscape: true
            highlighted: GridView.isCurrentItem && grid.activeFocus
            onClicked: page.Window.window.openItem(itemId, page)
            subtitle: {
                if (episodeNumber >= 0) {
                    const label = EpisodeLabel.label(seasonNumber, episodeNumber);
                    return episodeTitle.length > 0 ? i18nc("episode label and episode title", "%1 — %2", label, episodeTitle) : label;
                }
                return year > 0 ? String(year) : "";
            }
            spokenSubtitle: {
                if (episodeNumber >= 0) {
                    const label = EpisodeLabel.spoken(seasonNumber, episodeNumber);
                    return episodeTitle.length > 0 ? i18nc("episode label and episode title", "%1 — %2", label, episodeTitle) : label;
                }
                return year > 0 ? String(year) : "";
            }
        }
    }
}
