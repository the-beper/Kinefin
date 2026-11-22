// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The filters of a library list: one list of sections (status, features, video quality and type, series status, episodes, genres, tags, ratings, years), each shown only when the open scope offers it and its values are known. The choices are a draft that Apply writes to `library.filters` in one go; the genres, tags, ratings and years come from the server and are shown as plain text. The tags section is one row with a search field and a list of its own: a server can have thousands of tags, so the list shows the first ones and the search finds the rest
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
    // The search of the tags section and the tags the draft held when it last changed: a checked tag stays listed and checked whatever the search says, and a tag taken off stays until the next search so that the row does not vanish under the pointer
    property string tagSearch: ""
    property var keptTags: []
    readonly property var tagRows: buildTagRows()
    readonly property string tagsNote: buildTagsNote()

    title: i18n("Filters")
    padding: 0
    // Closing drops the choices not applied: Cancel says so
    standardButtons: Kirigami.Dialog.Cancel
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

    // Whether the entry may be switched: taking a choice off always may, any other choice is refused when the query would be longer than the client sends (a saved view can already be at the limit, and the model would drop the whole draft at Apply without a word)
    function canAdd(entry) {
        return isSelected(entry) || library.filtersFit(toggled(entry));
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
            return { Subtitles: i18nc("@option:check filter by feature", "Subtitles"), Trailer: i18nc("@option:check filter by feature", "Trailer"), SpecialFeatures: i18nc("@option:check filter by feature", "Special features"), ThemeSong: i18nc("@option:check filter by feature", "Theme song"), ThemeVideo: i18nc("@option:check filter by feature", "Theme video") }[value] ?? "";
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
                rows.push({ kind: "tags", key: "tags" });
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

    // The tag options of the tags section: `FilterOptionsModel.tagRows` picks the places of the server's list to show, the row keys keep those places so that a row keeps its name through a search
    function buildTagRows() {
        optionsRevision;
        const tags = options.tags;
        const rows = [];
        options.tagRows(tagSearch, keptTags).forEach(at => rows.push({ kind: "option", facet: "tags", value: tags[at], label: tags[at], key: "tags_" + at }));
        return rows;
    }

    // The line under the tag list: how many tags the first rows leave out, or that the search found none
    function buildTagsNote() {
        optionsRevision;
        if (tagSearch.trim() !== "")
            return tagRows.length === 0 ? i18n("No tags match the search") : "";
        const hidden = options.tagCount - tagRows.length;
        return hidden > 0 ? i18np("%1 more tag: use the search to find it", "%1 more tags: use the search to find them", hidden) : "";
    }

    onTagSearchChanged: keptTags = draft.tags.slice()

    // The next row that is an option, from the current one in the direction of `step`
    // The next row the arrows stop on: an option, or the tags section, which the arrows enter and leave like the options around it
    function nextOption(from, step) {
        for (let i = from + step; i >= 0 && i < entries.length; i += step) {
            if (entries[i].kind === "option" || entries[i].kind === "tags")
                return i;
        }
        return from;
    }
    function moveInList(step) {
        const next = nextOption(list.currentIndex, step);
        if (next === list.currentIndex)
            return;
        list.currentIndex = next;
        if (entries[next].kind === "tags") {
            list.positionViewAtIndex(next, ListView.Contain);
            list.currentItem?.item?.enter(step < 0);
        }
    }
    // From the tags section back to the options above or below it
    function leaveTags(tagsIndex, step) {
        const next = nextOption(tagsIndex, step);
        if (next === tagsIndex)
            return false;
        list.currentIndex = next;
        list.forceActiveFocus(step < 0 ? Qt.BacktabFocusReason : Qt.TabFocusReason);
        return true;
    }

    Connections {
        target: dialog.options
        function onOptionsChanged() {
            dialog.optionsRevision++;
        }
    }

    onAboutToShow: {
        draft = copyOf(library.filters);
        tagSearch = "";
        keptTags = draft.tags.slice();
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
        Keys.onUpPressed: dialog.moveInList(-1)
        Keys.onDownPressed: dialog.moveInList(1)
        // Space switches the option, Return applies the dialog as its default action
        Keys.onReturnPressed: dialog.apply()
        Keys.onEnterPressed: dialog.apply()
        Keys.onSpacePressed: toggleCurrent()

        delegate: Loader {
            id: row

            required property var modelData
            required property int index

            width: ListView.view ? ListView.view.width : implicitWidth
            // What the option rows of the list and of the tags section share: the entry, whether it is the current one and what a click does
            readonly property var entry: modelData
            readonly property bool current: ListView.isCurrentItem && list.activeFocus
            function activate() {
                list.currentIndex = index;
                dialog.toggle(modelData);
            }

            sourceComponent: {
                switch (row.modelData.kind) {
                case "heading":
                    return headingComponent;
                case "state":
                    return stateComponent;
                case "tags":
                    return tagsComponent;
                }
                return optionComponent;
            }

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

            // The row of an option, in the list or in the tags section: its Loader (the parent) has the `entry`, whether it is `current` and what `activate()` does
            Component {
                id: optionComponent

                Controls.ItemDelegate {
                    id: option
                    objectName: "filterOption_" + host.entry.key

                    readonly property var host: parent
                    readonly property bool selected: dialog.isSelected(host.entry)
                    // A value that would make the query too long for the server
                    readonly property bool blocked: !selected && !dialog.canAdd(host.entry)
                    readonly property string hint: blocked ? i18nc("@info hint on a filter value that cannot be added", "Too many filters selected: remove one to add this") : ""

                    enabled: !blocked
                    focusPolicy: Qt.NoFocus
                    highlighted: host.current
                    onClicked: host.activate()

                    // The label is server text for genres, tags and ratings: the control has no text of its own, so no mnemonic or markup applies
                    Accessible.role: Accessible.CheckBox
                    Accessible.name: host.entry.label
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
                            onClicked: option.host.activate()
                        }

                        Controls.Label {
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            textFormat: Text.PlainText
                            text: host.entry.label
                        }

                        Controls.Label {
                            visible: option.blocked
                            Layout.maximumWidth: option.width / 2
                            elide: Text.ElideRight
                            color: Kirigami.Theme.disabledTextColor
                            textFormat: Text.PlainText
                            text: option.hint
                        }
                    }
                }
            }

            Component {
                id: tagsComponent

                ColumnLayout {
                    spacing: Kirigami.Units.smallSpacing

                    // Down from the option above lands on the field, Up from the option below on the last tag
                    function enter(fromBelow) {
                        if (fromBelow && tagList.count > 0) {
                            tagList.currentIndex = tagList.count - 1;
                            tagList.forceActiveFocus(Qt.BacktabFocusReason);
                        } else {
                            tagField.forceActiveFocus(Qt.TabFocusReason);
                        }
                    }

                    Kirigami.SearchField {
                        id: tagField
                        objectName: "filterTagSearch"
                        Layout.fillWidth: true
                        Layout.leftMargin: Kirigami.Units.largeSpacing
                        Layout.rightMargin: Kirigami.Units.largeSpacing
                        // The shell owns Ctrl+F: the field's own sequence would make it ambiguous
                        focusSequences: []
                        maximumLength: 128
                        autoAccept: false
                        placeholderText: i18nc("@info:placeholder", "Search tags")
                        Accessible.name: i18nc("@info:placeholder", "Search tags")
                        text: dialog.tagSearch
                        onTextChanged: {
                            if (text !== dialog.tagSearch)
                                dialog.tagSearch = text;
                        }
                        Keys.onDownPressed: {
                            if (tagList.count > 0)
                                tagList.forceActiveFocus(Qt.TabFocusReason);
                            else
                                dialog.leaveTags(row.index, 1);
                        }
                        Keys.onUpPressed: dialog.leaveTags(row.index, -1)
                    }

                    ListView {
                        id: tagList
                        objectName: "filterTagList"

                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.min(contentHeight, Kirigami.Units.gridUnit * 12)
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        activeFocusOnTab: true
                        currentIndex: 0
                        model: dialog.tagRows
                        Accessible.role: Accessible.List
                        Accessible.name: i18nc("@info accessible name of the list of tags in the filter dialog", "Tag List")

                        Controls.ScrollBar.vertical: Controls.ScrollBar {}

                        function toggleCurrent() {
                            const entry = dialog.tagRows[tagList.currentIndex];
                            if (entry)
                                dialog.toggle(entry);
                        }
                        Keys.onUpPressed: {
                            if (tagList.currentIndex > 0)
                                tagList.currentIndex--;
                            else
                                tagField.forceActiveFocus(Qt.BacktabFocusReason);
                        }
                        Keys.onDownPressed: {
                            if (tagList.currentIndex < tagList.count - 1)
                                tagList.currentIndex++;
                            else
                                dialog.leaveTags(row.index, 1);
                        }
                        Keys.onReturnPressed: dialog.apply()
                        Keys.onEnterPressed: dialog.apply()
                        Keys.onSpacePressed: toggleCurrent()

                        delegate: Loader {
                            id: tagRow

                            required property var modelData
                            required property int index

                            readonly property var entry: modelData
                            readonly property bool current: ListView.isCurrentItem && tagList.activeFocus
                            function activate() {
                                tagList.currentIndex = index;
                                dialog.toggle(modelData);
                            }

                            width: ListView.view ? ListView.view.width : implicitWidth
                            sourceComponent: optionComponent
                        }
                    }

                    Controls.Label {
                        objectName: "filterTagsNote"
                        Layout.fillWidth: true
                        Layout.leftMargin: Kirigami.Units.largeSpacing
                        Layout.rightMargin: Kirigami.Units.largeSpacing
                        visible: text !== ""
                        wrapMode: Text.Wrap
                        color: Kirigami.Theme.disabledTextColor
                        textFormat: Text.PlainText
                        text: dialog.tagsNote
                    }
                }
            }
        }
    }
}
