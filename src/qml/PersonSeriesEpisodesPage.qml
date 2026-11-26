// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The episodes one person has in one series, by season: opened from a series card of the person page. The list is the one the person's model filled for the series
Kirigami.ScrollablePage {
    id: page
    objectName: "personSeriesEpisodesPage"

    // The darker View background on every page, as the person page has it
    Kirigami.Theme.colorSet: Kirigami.Theme.View
    Kirigami.Theme.inherit: false

    required property ConnectionController connection
    required property string seriesName
    required property string personName

    readonly property PersonEpisodesModel model: connection.personItems.seriesEpisodes
    readonly property real margin: PageMetrics.margin
    readonly property real listColumnWidth: Math.min(Math.max(0, list.width - margin * 2), page.Window.window?.readingWidth ?? Infinity)

    // Server text: the page row shows it as plain text
    title: i18nc("page title: a series and a person in it", "%1 · %2", seriesName, personName)
    // The row of the page carries the way back and the name, as on the person page
    globalToolBarStyle: Kirigami.ApplicationHeaderStyle.None
    // Without Kirigami's title row the page names itself to a screen reader
    Accessible.role: Accessible.Pane
    Accessible.name: title

    // The list takes the keys as soon as the page shows
    function focusPrimary() {
        list.forceActiveFocus(Qt.TabFocusReason);
    }

    // Season 0 is the specials, a season without a number comes last
    function seasonTitle(season) {
        if (season > 0)
            return i18n("Season %1", season);
        return season === 0 ? i18n("Specials") : i18n("Season");
    }

    header: PageToolbar {
        backShown: true
        title: page.title
    }

    ListView {
        id: list
        // What scrolls under the page row fades into it instead of being cut by its edge
        TopShadow {
            parent: list
            z: 1
            atStart: list.atYBeginning
        }
        objectName: "personSeriesEpisodesList"
        // The season heading holds the room under the bar, as on the Downloads page: the style's own padding of a heading would double it
        topMargin: 0
        bottomMargin: page.margin

        model: page.model
        activeFocusOnTab: true
        keyNavigationEnabled: true
        currentIndex: 0
        Accessible.role: Accessible.List
        Accessible.name: page.title

        // The rows are not tab stops: the list is, and it opens the current episode
        function openCurrent() {
            const row = list.currentItem?.episode;
            if (row?.enabled)
                page.Window.window.openItem(row.itemId, page);
        }
        Keys.onReturnPressed: openCurrent()
        Keys.onEnterPressed: openCurrent()
        Keys.onSpacePressed: openCurrent()

        section.property: "seasonNumber"
        section.delegate: Item {
            width: list.width
            height: sectionHeader.implicitHeight

            Kirigami.ListSectionHeader {
                id: sectionHeader
                objectName: "personSeason_" + section
                x: (list.width - page.listColumnWidth) / 2
                width: page.listColumnWidth
                topPadding: page.margin
                text: page.seasonTitle(Number(section))
            }
        }

        // A slot as wide as the list centres the row in it: the list resets the x of a delegate it lays out, so only the first row kept a centring of its own
        delegate: Item {
            id: episodeSlot
            readonly property alias episode: episodeRow
            width: list.width
            height: episodeRow.height

            EpisodeRow {
                id: episodeRow
                x: (episodeSlot.width - width) / 2
                width: page.listColumnWidth
                // The list is the one tab stop, the arrow keys move inside it
                focusPolicy: Qt.NoFocus
                highlighted: episodeSlot.ListView.isCurrentItem && list.activeFocus
                itemId: model.itemId
                name: model.name
                seasonNumber: model.seasonNumber
                episodeNumber: model.episodeNumber
                episodeNumberEnd: model.episodeNumberEnd
                runtimeMinutes: model.runtimeMinutes
                overview: model.overview
                imageUrl: model.imageUrl
                played: model.played
                playedPercentage: model.playedPercentage
                favorite: model.favorite
                virtualItem: model.virtualItem
                unaired: model.unaired
                loaded: model.loaded
                onClicked: page.Window.window.openItem(itemId, page)
            }
        }
    }
}
