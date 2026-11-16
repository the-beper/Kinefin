// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The filters of a library list: one list of sections (status, features, video quality and type, series status, episodes, genres, tags, ratings, years), each shown only when the open scope offers it and its values are known. The choices are a draft that Apply writes to `library.filters` in one go; the genres, tags, ratings and years come from the server and are shown as plain text
Kirigami.Dialog {
    id: dialog
    objectName: "filterDialog"

    required property LibraryItemsModel library
    // The list is the favorites already (the Favorites sub-tab): there is no choice to make
    property bool favoritesFixed: false

    readonly property FilterOptionsModel options: library.filterOptions
    // The filters as the model keeps them, edited until Apply; replaced as a whole on every edit so bindings re-evaluate
    property var draft: emptyFilters()
    // The facet lists changed: the rows of the genre list are read with data(), which no binding tracks
    property int optionsRevision: 0
    readonly property bool draftIsEmpty: isEmptyFilters(draft)
    readonly property var entries: buildEntries()

    title: i18n("Filters")
    padding: 0
    standardButtons: Kirigami.Dialog.Close
    customFooterActions: [
        Kirigami.Action {
            objectName: "filterClearAction"
            text: i18n("Clear")
            icon.name: "edit-clear-all"
            enabled: !dialog.draftIsEmpty
            onTriggered: dialog.draft = dialog.emptyFilters()
        },
        Kirigami.Action {
            objectName: "filterApplyAction"
            text: i18n("Apply")
            icon.name: "dialog-ok-apply"
            onTriggered: dialog.apply()
        }
    ]

    // The keys and tokens of LibraryItemsModel::filters
    function emptyFilters() {
        return { played: "Any", resumable: false, favorites: false, features: [], quality: "Any", is4K: false, is3D: false, videoTypes: [], genreIds: [], tags: [], officialRatings: [], years: [], seriesStatus: [], episodeKind: "All" };
    }

    // The filters of the model as plain arrays and values: what the model hands over is not changed in place
    function copyOf(filters) {
        const copy = emptyFilters();
        for (const key of Object.keys(copy))
            copy[key] = Array.isArray(copy[key]) ? Array.from(filters[key] ?? []) : (filters[key] ?? copy[key]);
        return copy;
    }

    function isEmptyFilters(filters) {
        const empty = emptyFilters();
        return Object.keys(empty).every(key => Array.isArray(empty[key]) ? filters[key].length === 0 : filters[key] === empty[key]);
    }

    function apply() {
        library.filters = draft;
        close();
    }

    function isSelected(entry) {
        switch (entry.facet) {
        case "played":
        case "quality":
        case "episodeKind":
            return draft[entry.facet] === entry.value;
        case "resumable":
        case "favorites":
        case "is4K":
        case "is3D":
            return draft[entry.facet] === true;
        }
        return draft[entry.facet].indexOf(entry.value) >= 0;
    }

    // Played and Unplayed, HD and SD, and the kinds of episodes exclude each other (an AND of them could never match), and 4K is HD
    function toggle(entry) {
        if (canAdd(entry))
            draft = toggled(entry);
    }

    // Whether the entry may be switched: taking a choice off always may, a value of a list section (genre, tag, rating, year) is refused when the query would be longer than the client sends
    function canAdd(entry) {
        switch (entry.facet) {
        case "genreIds":
        case "tags":
        case "officialRatings":
        case "years":
            return isSelected(entry) || library.filtersFit(toggled(entry));
        }
        return true;
    }

    // The draft with the entry switched
    function toggled(entry) {
        const next = copyOf(draft);
        const on = !isSelected(entry);
        switch (entry.facet) {
        case "played":
            next.played = on ? entry.value : "Any";
            break;
        case "quality":
            next.quality = on ? entry.value : "Any";
            if (on && entry.value === "SD")
                next.is4K = false;
            break;
        case "episodeKind":
            next.episodeKind = on ? entry.value : "All";
            break;
        case "is4K":
            next.is4K = on;
            if (on && next.quality === "SD")
                next.quality = "Any";
            break;
        case "resumable":
        case "favorites":
        case "is3D":
            next[entry.facet] = on;
            break;
        default:
            {
                const values = next[entry.facet];
                const at = values.indexOf(entry.value);
                if (at >= 0)
                    values.splice(at, 1);
                else
                    values.push(entry.value);
            }
        }
        return next;
    }

    // The text of an option or chip: the fixed ones are translated, tags, ratings and years show as they are, a genre by the name the server lists for its id (a genre whose list failed to load shows as "Genre")
    function labelOf(facet, value) {
        switch (facet) {
        case "played":
            return value === "Played" ? i18nc("@option:check filter by watched state", "Played") : i18nc("@option:check filter by watched state", "Unplayed");
        case "resumable":
            return i18nc("@option:check filter by watched state", "Resumable");
        case "favorites":
            return i18nc("@option:check filter by favorite", "Favorites");
        case "features":
            return { Subtitles: i18nc("@option:check filter by feature", "Subtitles"), Trailer: i18nc("@option:check filter by feature", "Trailer"), SpecialFeatures: i18nc("@option:check filter by feature", "Special Features"), ThemeSong: i18nc("@option:check filter by feature", "Theme Song"), ThemeVideo: i18nc("@option:check filter by feature", "Theme Video") }[value] ?? "";
        case "quality":
            return value === "HD" ? i18nc("@option:check filter by video quality", "HD") : i18nc("@option:check filter by video quality", "SD");
        case "is4K":
            return i18nc("@option:check filter by video quality", "4K");
        case "is3D":
            return i18nc("@option:check filter by video quality", "3D");
        case "videoTypes":
            return value === "BluRay" ? i18nc("@option:check filter by video type", "Blu-ray") : i18nc("@option:check filter by video type", "DVD");
        case "seriesStatus":
            return { Continuing: i18nc("@option:check filter by series status", "Continuing"), Ended: i18nc("@option:check filter by series status", "Ended"), Unreleased: i18nc("@option:check filter by series status", "Unreleased") }[value] ?? "";
        case "episodeKind":
            return { Specials: i18nc("@option:check filter by kind of episode", "Specials"), Missing: i18nc("@option:check filter by kind of episode", "Missing"), Unaired: i18nc("@option:check filter by kind of episode", "Unaired") }[value] ?? "";
        case "genreIds":
            return genreName(value) || i18nc("@item:inlistbox a genre whose name is not known", "Genre");
        }
        return String(value);
    }

    // FilterOptionsModel::GenreIdRole and NameRole are Qt::UserRole + 1 and + 2
    function genreName(genreId) {
        for (let row = 0; row < options.rowCount(); ++row) {
            const index = options.index(row, 0);
            if (options.data(index, Qt.UserRole + 1) === genreId)
                return options.data(index, Qt.UserRole + 2);
        }
        return "";
    }

    // The filters the model has now, one chip each: `facet`, `value` and the `label` to show
    readonly property var chips: buildChips()

    function buildChips() {
        optionsRevision;
        const filters = copyOf(library.filters);
        const chips = [];
        const add = (facet, value) => chips.push({ facet: facet, value: value, label: labelOf(facet, value) });
        if (filters.played !== "Any")
            add("played", filters.played);
        for (const facet of ["resumable", "favorites"]) {
            if (filters[facet])
                add(facet, true);
        }
        filters.features.forEach(feature => add("features", feature));
        if (filters.quality !== "Any")
            add("quality", filters.quality);
        for (const facet of ["is4K", "is3D"]) {
            if (filters[facet])
                add(facet, true);
        }
        for (const facet of ["videoTypes", "seriesStatus", "genreIds", "tags", "officialRatings", "years"])
            filters[facet].forEach(value => add(facet, value));
        if (filters.episodeKind !== "All")
            add("episodeKind", filters.episodeKind);
        return chips;
    }

    // Takes one chip's filter off the model at once, without the dialog
    function removeChip(chip) {
        const next = copyOf(library.filters);
        switch (chip.facet) {
        case "played":
            next.played = "Any";
            break;
        case "quality":
            next.quality = "Any";
            break;
        case "episodeKind":
            next.episodeKind = "All";
            break;
        case "resumable":
        case "favorites":
        case "is4K":
        case "is3D":
            next[chip.facet] = false;
            break;
        default:
            next[chip.facet] = next[chip.facet].filter(value => value !== chip.value);
        }
        library.filters = next;
    }

    // The rows of the dialog's list: headings, options and the state of a facet source that is loading or failed. `facet` and `value` name the filter, `label` is what shows (server text for genres, tags, ratings), `key` makes the object name
    function buildEntries() {
        optionsRevision;
        const rows = [];
        const heading = title => rows.push({ kind: "heading", title: title, key: title });
        const option = (facet, value, key, label) => rows.push({ kind: "option", facet: facet, value: value, label: label ?? labelOf(facet, value), key: key ?? facet + "_" + value });
        const state = (facet, loading) => rows.push({ kind: "state", loading: loading, key: facet });

        heading(i18nc("@title:group section of the filter dialog", "Status"));
        option("played", "Played");
        option("played", "Unplayed");
        if (library.resumableAvailable)
            option("resumable", true);
        if (!favoritesFixed)
            option("favorites", true);

        if (library.featuresAvailable) {
            heading(i18nc("@title:group section of the filter dialog", "Features"));
            option("features", "Subtitles");
            option("features", "Trailer");
            option("features", "SpecialFeatures");
            option("features", "ThemeSong");
            option("features", "ThemeVideo");
        }

        if (library.videoQualityAvailable) {
            heading(i18nc("@title:group section of the filter dialog", "Video Quality"));
            option("quality", "HD");
            option("quality", "SD");
            option("is4K", true);
            option("is3D", true);
            heading(i18nc("@title:group section of the filter dialog: Blu-ray and DVD", "Video Type"));
            option("videoTypes", "BluRay");
            option("videoTypes", "Dvd");
        }

        if (library.seriesStatusAvailable) {
            heading(i18nc("@title:group section of the filter dialog", "Series Status"));
            option("seriesStatus", "Continuing");
            option("seriesStatus", "Ended");
            option("seriesStatus", "Unreleased");
        }

        if (library.episodeKindAvailable) {
            heading(i18nc("@title:group section of the filter dialog", "Episodes"));
            option("episodeKind", "Specials");
            option("episodeKind", "Missing");
            option("episodeKind", "Unaired");
        }

        // The genres and the tags come from one request, the ratings and the years from another: each pair has its own loading and failure state, and a server without the second hides its sections without a word
        const source = options.status;
        if (source === FilterOptionsModel.Loading || source === FilterOptionsModel.Failed) {
            heading(i18nc("@title:group section of the filter dialog", "Genres and Tags"));
            state("genres", source === FilterOptionsModel.Loading);
        } else if (source === FilterOptionsModel.Ready) {
            if (options.available) {
                heading(i18nc("@title:group section of the filter dialog", "Genres"));
                for (let row = 0; row < options.rowCount(); ++row) {
                    const index = options.index(row, 0);
                    option("genreIds", options.data(index, Qt.UserRole + 1), undefined, options.data(index, Qt.UserRole + 2));
                }
            }
            if (options.tagsAvailable) {
                heading(i18nc("@title:group section of the filter dialog", "Tags"));
                options.tags.forEach((tag, at) => option("tags", tag, "tags_" + at));
            }
        }
        const legacy = options.legacyStatus;
        if (legacy === FilterOptionsModel.Loading || legacy === FilterOptionsModel.Failed) {
            heading(i18nc("@title:group section of the filter dialog", "Ratings and Years"));
            state("legacy", legacy === FilterOptionsModel.Loading);
        } else if (legacy === FilterOptionsModel.Ready) {
            if (options.ratingsAvailable) {
                heading(i18nc("@title:group section of the filter dialog", "Ratings"));
                options.officialRatings.forEach((rating, at) => option("officialRatings", rating, "officialRatings_" + at));
            }
            if (options.yearsAvailable) {
                heading(i18nc("@title:group section of the filter dialog", "Years"));
                options.years.forEach(year => option("years", year));
            }
        }
        return rows;
    }

    // The next row that is an option, from the current one in the direction of `step`
    function nextOption(from, step) {
        for (let i = from + step; i >= 0 && i < entries.length; i += step) {
            if (entries[i].kind === "option")
                return i;
        }
        return from;
    }

    Connections {
        target: dialog.options
        function onOptionsChanged() {
            dialog.optionsRevision++;
        }
    }

    onAboutToShow: {
        draft = copyOf(library.filters);
        list.model = Qt.binding(() => dialog.entries);
    }
    onClosed: list.model = []
    onOpened: {
        list.currentIndex = dialog.nextOption(-1, 1);
        list.forceActiveFocus(Qt.PopupFocusReason);
    }

    ListView {
        id: list
        objectName: "filterOptionsList"

        implicitWidth: Kirigami.Units.gridUnit * 22
        implicitHeight: Math.min(contentHeight, Kirigami.Units.gridUnit * 24)
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        activeFocusOnTab: true
        currentIndex: 0
        // Rows only while the dialog shows (set when it opens and dropped when it closes): the page creates the dialog with it, and a closed one would rebuild its rows every time the filter values arrive
        model: []
        Accessible.role: Accessible.List
        Accessible.name: i18n("Filters")

        Controls.ScrollBar.vertical: Controls.ScrollBar {}

        // The rows are not tab stops: the list is, and it moves over the options and toggles the current one
        function toggleCurrent() {
            const entry = dialog.entries[list.currentIndex];
            if (entry?.kind === "option")
                dialog.toggle(entry);
        }
        Keys.onUpPressed: list.currentIndex = dialog.nextOption(list.currentIndex, -1)
        Keys.onDownPressed: list.currentIndex = dialog.nextOption(list.currentIndex, 1)
        Keys.onReturnPressed: toggleCurrent()
        Keys.onEnterPressed: toggleCurrent()
        Keys.onSpacePressed: toggleCurrent()

        delegate: Loader {
            id: row

            required property var modelData
            required property int index

            width: ListView.view ? ListView.view.width : implicitWidth
            sourceComponent: row.modelData.kind === "heading" ? headingComponent : (row.modelData.kind === "state" ? stateComponent : optionComponent)

            Component {
                id: headingComponent

                Kirigami.Heading {
                    topPadding: Kirigami.Units.largeSpacing
                    leftPadding: Kirigami.Units.largeSpacing
                    level: 4
                    Accessible.role: Accessible.Heading
                    text: row.modelData.title
                }
            }

            Component {
                id: stateComponent

                RowLayout {
                    spacing: Kirigami.Units.largeSpacing

                    Controls.BusyIndicator {
                        FadeInOnShow {}
                        Layout.leftMargin: Kirigami.Units.largeSpacing
                        running: visible
                        visible: row.modelData.loading
                        Accessible.name: i18n("Loading…")
                    }

                    Controls.Label {
                        Layout.fillWidth: true
                        Layout.leftMargin: Kirigami.Units.largeSpacing
                        wrapMode: Text.Wrap
                        text: row.modelData.loading ? i18n("Loading…") : i18n("Could not load these filters")
                    }

                    Controls.Button {
                        objectName: "filterRetry_" + row.modelData.key
                        Layout.rightMargin: Kirigami.Units.largeSpacing
                        visible: !row.modelData.loading
                        text: i18n("Retry")
                        icon.name: "view-refresh"
                        onClicked: dialog.options.retry()
                    }
                }
            }

            Component {
                id: optionComponent

                Controls.ItemDelegate {
                    id: option
                    objectName: "filterOption_" + row.modelData.key

                    readonly property bool selected: dialog.isSelected(row.modelData)
                    // A value that would make the query too long for the server
                    readonly property bool blocked: !selected && !dialog.canAdd(row.modelData)
                    readonly property string hint: blocked ? i18nc("@info hint on a filter value that cannot be added", "Too many filters selected: remove one to add this") : ""

                    enabled: !blocked
                    focusPolicy: Qt.NoFocus
                    highlighted: row.ListView.isCurrentItem && list.activeFocus
                    onClicked: {
                        list.currentIndex = row.index;
                        dialog.toggle(row.modelData);
                    }

                    // The label is server text for genres, tags and ratings: the control has no text of its own, so no mnemonic or markup applies
                    Accessible.role: Accessible.CheckBox
                    Accessible.name: row.modelData.label
                    Accessible.checkable: true
                    Accessible.checked: selected
                    Accessible.description: hint

                    contentItem: RowLayout {
                        spacing: Kirigami.Units.largeSpacing

                        Controls.CheckBox {
                            focusPolicy: Qt.NoFocus
                            checked: option.selected
                            Accessible.ignored: true
                            // Only a user click edits the draft: a changed selection also updates `checked`, which emits toggled and must not invert the selection again
                            onClicked: {
                                list.currentIndex = row.index;
                                dialog.toggle(row.modelData);
                            }
                        }

                        Controls.Label {
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            textFormat: Text.PlainText
                            text: row.modelData.label
                        }

                        Controls.Label {
                            visible: option.blocked
                            Layout.maximumWidth: row.width / 2
                            elide: Text.ElideRight
                            color: Kirigami.Theme.disabledTextColor
                            textFormat: Text.PlainText
                            text: option.hint
                        }
                    }
                }
            }
        }
    }
}
