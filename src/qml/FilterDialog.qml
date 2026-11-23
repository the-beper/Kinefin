// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.delegates as Delegates

// The filters of a library list or of the favorites: a list of facets (status, features, video quality and type, series status, episodes, genres, tags, ratings, years), each shown only when the open scope offers it and its values are known, beside a pane with the values of the facet that is current. The choices are a draft that Apply writes to `list.filters` in one go; the genres, tags, ratings and years come from the server and are shown as plain text, with a search field over them (a server can have thousands of tags: the pane lists the first ones and the search finds the rest). In a window too narrow for both panes the facet list shows alone and a facet opens in its place
Kirigami.Dialog {
    id: dialog
    objectName: "filterDialog"

    // A LibraryItemsModel or the FavoritesModel: both have `filters`, `filterOptions` and `filtersFit()`, and a library says which other facets its scope offers with `featuresAvailable`, `videoQualityAvailable`, `seriesStatusAvailable`, `episodeKindAvailable` and `resumableAvailable`, which a model without them does not offer (a plain var, so that qmllint cannot check the members)
    required property var list
    // The list is the favorites already (the Favorites sub-tab, the Favorites pages): there is no choice to make
    property bool favoritesFixed: false
    // The id of a genre is its name (the favorites are filtered by the names they carry): a chip of a genre the options do not list yet shows the id
    property bool genreIdsAreNames: false

    readonly property FilterOptionsModel options: list.filterOptions
    // The favorites have no flag for it and always have a saved position to look at
    readonly property bool resumableOffered: list.resumableAvailable ?? true
    // The filters as the model keeps them, edited until Apply; replaced as a whole on every edit so bindings re-evaluate
    property var draft: emptyFilters()
    // The facet lists changed: the rows of the genre list are read with data(), which no binding tracks
    property int optionsRevision: 0
    readonly property bool draftIsEmpty: isEmptyFilters(draft)

    // The entries of the facet list: `key`, `title`, and `busy` or `failed` while the source of the facet loads or has failed
    readonly property var facets: buildFacets()
    // What the pane shows: the facet, the search over it and the values the draft held in the facet when the search last changed (they stay listed and checked whatever the search says, and a value taken off stays until the next search so that the row does not vanish under the pointer). Always replaced as a whole, so a change of facet or of search rebuilds the rows once
    property var pane: ({ facet: "", text: "", kept: [] })
    readonly property string currentFacet: pane.facet
    readonly property int facetIndex: facets.findIndex(facet => facet.key === pane.facet)
    readonly property var facetEntry: facets[facetIndex] ?? null
    // The filters a facet with a search field changes
    readonly property var searchFields: ({ genres: "genreIds", tags: "tags", ratings: "officialRatings", years: "years" })
    readonly property bool searchable: searchFields[pane.facet] !== undefined
    // The entries that stand in for one another while a source loads, has failed or answers
    readonly property var facetFamilies: [["genresAndTags", "genres", "tags"], ["ratingsAndYears", "ratings", "years"]]
    // Rows only while the dialog shows (set when it opens and dropped when it closes): the page creates the dialog with it, and a closed one would rebuild its rows every time the filter values arrive
    property bool built: false
    readonly property var rows: built ? optionRows(pane.facet) : []
    readonly property string note: buildNote()
    // A search that finds nothing: the pane says so in its middle, as for a source that is loading or failed
    readonly property bool noMatch: searchable && pane.text.trim() !== "" && rows.length === 0

    readonly property bool rtl: Qt.application.layoutDirection === Qt.RightToLeft
    // The two panes sit side by side, or one at a time when the window cannot give the dialog its width
    readonly property bool narrow: absoluteMaximumWidth < preferredWidth
    // Narrow windows only: the facet shows in place of the facet list
    property bool facetOpen: false
    // Escape clears a search first, and closes an open facet of a narrow window before it closes the dialog
    readonly property bool escapeUsed: pane.text !== "" || (narrow && facetOpen)
    // The same height whatever the facet: the room the window leaves is a limit too, and the opening animation starts the dialog two grid units lower, so it must not reach the window edge while it moves
    readonly property real totalHeight: Math.min(Kirigami.Units.gridUnit * 26, absoluteMaximumHeight - Kirigami.Units.gridUnit * 4)
    readonly property real bodyHeight: Math.max(Kirigami.Units.gridUnit * 8, totalHeight - implicitHeaderHeight - implicitFooterHeight)

    title: i18n("Filters")
    preferredWidth: Kirigami.Units.gridUnit * 32
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

    header: DialogTitleBar {
        title: dialog.title
        onCloseRequested: dialog.close()
    }

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
        list.filters = draft;
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
        return isSelected(entry) || list.filtersFit(toggled(entry));
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

    // The text of an option or chip: the fixed ones are translated, tags, ratings and years show as they are, a genre by the name the server lists for its id (a genre whose list failed to load shows as "Genre", or by its id where the id is the name). The `facet` of an entry or a chip is the key of the filter it changes, not an entry of the facet list
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
            return genreName(value) || (genreIdsAreNames ? String(value) : i18nc("@item:inlistbox a genre whose name is not known", "Genre"));
        }
        return String(value);
    }

    function genreName(genreId) {
        for (let row = 0; row < options.rowCount(); ++row) {
            const index = options.index(row, 0);
            if (options.data(index, FilterOptionsModel.GenreIdRole) === genreId)
                return options.data(index, FilterOptionsModel.NameRole);
        }
        return "";
    }

    // The filters the model has now, one chip each: `facet`, `value` and the `label` to show
    readonly property var chips: buildChips()

    function buildChips() {
        optionsRevision;
        const filters = copyOf(list.filters);
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
        const next = copyOf(list.filters);
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
        list.filters = next;
    }

    // The entries of the facet list, in the order of the dialog. The genres and the tags come from one request, the ratings and the years from another: each pair has its own loading and failure entry, and a server without the second hides its facets without a word
    function buildFacets() {
        optionsRevision;
        const facets = [];
        const add = (key, title, icon, state) => facets.push({ key: key, title: title, icon: icon, busy: state === FilterOptionsModel.Loading, failed: state === FilterOptionsModel.Failed });
        add("status", i18nc("@title:group section of the filter dialog", "Status"), "view-visible-symbolic");
        if (list.featuresAvailable ?? false)
            add("features", i18nc("@title:group section of the filter dialog", "Features"), "view-list-details-symbolic");
        if (list.videoQualityAvailable ?? false) {
            add("quality", i18nc("@title:group section of the filter dialog", "Video Quality"), "video-television-symbolic");
            add("videoType", i18nc("@title:group section of the filter dialog: Blu-ray and DVD", "Video Type"), "media-optical-symbolic");
        }
        if (list.seriesStatusAvailable ?? false)
            add("seriesStatus", i18nc("@title:group section of the filter dialog", "Series Status"), "view-time-schedule-symbolic");
        if (list.episodeKindAvailable ?? false)
            add("episodes", i18nc("@title:group section of the filter dialog", "Episodes"), "view-media-playlist-symbolic");

        const source = options.status;
        if (source === FilterOptionsModel.Loading || source === FilterOptionsModel.Failed) {
            add("genresAndTags", i18nc("@title:group section of the filter dialog", "Genres and Tags"), "tag-symbolic", source);
        } else if (source === FilterOptionsModel.Ready) {
            if (options.available)
                add("genres", i18nc("@title:group section of the filter dialog", "Genres"), "view-media-genre-symbolic");
            if (options.tagsAvailable)
                add("tags", i18nc("@title:group section of the filter dialog", "Tags"), "tag-symbolic");
        }
        const legacy = options.legacyStatus;
        if (legacy === FilterOptionsModel.Loading || legacy === FilterOptionsModel.Failed) {
            add("ratingsAndYears", i18nc("@title:group section of the filter dialog", "Ratings and Years"), "starred-symbolic", legacy);
        } else if (legacy === FilterOptionsModel.Ready) {
            if (options.ratingsAvailable)
                add("ratings", i18nc("@title:group section of the filter dialog", "Ratings"), "starred-symbolic");
            if (options.yearsAvailable)
                add("years", i18nc("@title:group section of the filter dialog", "Years"), "view-calendar-symbolic");
        }
        return facets;
    }

    // How many values the draft checks in a facet
    function countOf(facetKey) {
        switch (facetKey) {
        case "status":
            return (draft.played !== "Any" ? 1 : 0) + (draft.resumable ? 1 : 0) + (draft.favorites ? 1 : 0);
        case "features":
            return draft.features.length;
        case "quality":
            return (draft.quality !== "Any" ? 1 : 0) + (draft.is4K ? 1 : 0) + (draft.is3D ? 1 : 0);
        case "videoType":
            return draft.videoTypes.length;
        case "seriesStatus":
            return draft.seriesStatus.length;
        case "episodes":
            return draft.episodeKind !== "All" ? 1 : 0;
        }
        const field = searchFields[facetKey];
        return field ? draft[field].length : 0;
    }

    // The values the draft holds in a facet that has a search field
    function heldValues(facetKey) {
        const field = searchFields[facetKey];
        return field ? draft[field].slice() : [];
    }

    // The facet that takes the place of one that went: the one that stands in for it, or the first
    function replacementFacet(facetKey) {
        const family = facetFamilies.find(keys => keys.indexOf(facetKey) >= 0) ?? [];
        return family.find(key => facets.some(facet => facet.key === key)) ?? facets[0]?.key ?? "";
    }

    function showFacet(facetKey) {
        if (facetKey !== pane.facet && facets.some(facet => facet.key === facetKey))
            pane = { facet: facetKey, text: "", kept: heldValues(facetKey) };
    }

    function searchFor(text) {
        if (text !== pane.text)
            pane = { facet: pane.facet, text: text, kept: heldValues(pane.facet) };
    }

    // The arrow keys of the facet list: the pane follows the facet
    function moveFacet(step) {
        const next = facets[Math.max(0, Math.min(facets.length - 1, facetIndex + step))];
        if (next)
            showFacet(next.key);
        facetList.positionViewAtIndex(Math.max(0, facetIndex), ListView.Contain);
    }

    // A click on a facet: in a narrow window the facet opens in place of the list and takes the focus
    function openFacet(facetKey) {
        showFacet(facetKey);
        if (narrow)
            enterPane();
        else
            facetList.forceActiveFocus(Qt.MouseFocusReason);
    }

    // The focus goes to the first thing of the pane that can have it: the search field, the values, or the button of a facet that failed
    function enterPane() {
        if (narrow)
            facetOpen = true;
        const target = [searchField, optionsList, retryButton, backButton].find(item => item.visible && item.enabled);
        target?.forceActiveFocus(Qt.TabFocusReason);
    }

    function leavePane() {
        if (narrow)
            facetOpen = false;
        facetList.forceActiveFocus(Qt.BacktabFocusReason);
    }

    // The rows of a facet: `facet` and `value` name the filter, `label` is what shows (server text for genres, tags, ratings), `key` makes the object name. The facets with a search field list the values that match it and the ones the draft held
    function optionRows(facetKey) {
        optionsRevision;
        const rows = [];
        const option = (facet, value, key, label) => rows.push({ facet: facet, value: value, label: label ?? labelOf(facet, value), key: key ?? facet + "_" + value });
        const needle = pane.text.trim().toLowerCase();
        const kept = new Set(pane.kept);
        const listed = (value, label) => needle === "" || label.toLowerCase().indexOf(needle) >= 0 || kept.has(value);

        switch (facetKey) {
        case "status":
            option("played", "Played");
            option("played", "Unplayed");
            if (resumableOffered)
                option("resumable", true);
            if (!favoritesFixed)
                option("favorites", true);
            break;
        case "features":
            for (const feature of ["Subtitles", "Trailer", "SpecialFeatures", "ThemeSong", "ThemeVideo"])
                option("features", feature);
            break;
        case "quality":
            option("quality", "HD");
            option("quality", "SD");
            option("is4K", true);
            option("is3D", true);
            break;
        case "videoType":
            option("videoTypes", "BluRay");
            option("videoTypes", "Dvd");
            break;
        case "seriesStatus":
            for (const status of ["Continuing", "Ended", "Unreleased"])
                option("seriesStatus", status);
            break;
        case "episodes":
            for (const kind of ["Specials", "Missing", "Unaired"])
                option("episodeKind", kind);
            break;
        case "genres":
            for (let row = 0; row < options.rowCount(); ++row) {
                const index = options.index(row, 0);
                const genreId = options.data(index, FilterOptionsModel.GenreIdRole);
                const name = options.data(index, FilterOptionsModel.NameRole);
                // Where the id is the name the object name takes the place of the row, never the text
                if (listed(genreId, name))
                    option("genreIds", genreId, genreIdsAreNames ? "genreIds_" + row : undefined, name);
            }
            break;
        case "tags":
            return tagOptionRows();
        case "ratings":
            options.officialRatings.forEach((rating, at) => {
                if (listed(rating, rating))
                    option("officialRatings", rating, "officialRatings_" + at);
            });
            break;
        case "years":
            options.years.forEach(year => {
                if (listed(year, String(year)))
                    option("years", year);
            });
            break;
        }
        return rows;
    }

    // The tag rows: `FilterOptionsModel.tagRows` picks the places of the server's list to show, the row keys keep those places so that a row keeps its name through a search; the kept tags come first, in the server's order, then the others
    function tagOptionRows() {
        const tags = options.tags;
        const kept = new Set(pane.kept);
        const first = [];
        const rest = [];
        options.tagRows(pane.text, pane.kept).forEach(at => (kept.has(tags[at]) ? first : rest).push(at));
        return first.concat(rest).map(at => ({ facet: "tags", value: tags[at], label: tags[at], key: "tags_" + at }));
    }

    // The line under the values: how many tags the first rows leave out, or that the search found nothing
    function buildNote() {
        optionsRevision;
        if (!searchable)
            return "";
        if (pane.text.trim() !== "")
            return rows.length === 0 ? i18nc("@info:placeholder", "No values match the search") : "";
        if (pane.facet === "tags") {
            const hidden = options.tagCount - rows.length;
            return hidden > 0 ? i18np("%1 more tag: use the search to find it", "%1 more tags: use the search to find them", hidden) : "";
        }
        return "";
    }

    function toggleCurrentRow() {
        const entry = rows[optionsList.currentIndex];
        if (entry)
            toggle(entry);
    }

    // A printable key typed in the values starts or continues the search, with that character
    function typeIntoSearch(event) {
        const code = event.text.length > 0 ? event.text.charCodeAt(0) : 0;
        if (code < 32 || code === 127 || (event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier)) || !searchField.visible)
            return;
        searchField.forceActiveFocus(Qt.TabFocusReason);
        searchField.insert(searchField.length, event.text);
        searchField.cursorPosition = searchField.length;
        event.accepted = true;
    }

    // Escape clears the search and, in a narrow window, closes the open facet; the item that has the focus claims the key before the shortcut with which the dialog closes on it, and only then
    function keepEscape(event) {
        event.accepted = event.key === Qt.Key_Escape && escapeUsed;
    }

    function handleEscape(event) {
        if (pane.text !== "")
            searchFor("");
        else if (narrow && facetOpen)
            leavePane();
        else
            event.accepted = false;
    }

    // A source answered or failed under the facet that shows: it moves to the one that takes its place
    onFacetsChanged: {
        if (pane.facet !== "" && !facets.some(facet => facet.key === pane.facet))
            showFacet(replacementFacet(pane.facet));
    }
    onNarrowChanged: {
        facetOpen = false;
        if (opened)
            Qt.callLater(() => facetList.forceActiveFocus(Qt.OtherFocusReason));
    }

    Connections {
        target: dialog.options
        function onOptionsChanged() {
            dialog.optionsRevision++;
        }
    }

    onAboutToShow: {
        draft = copyOf(list.filters);
        facetOpen = false;
        pane = { facet: facets[0]?.key ?? "", text: "", kept: [] };
        built = true;
    }
    onClosed: {
        built = false;
        facetOpen = false;
    }
    onOpened: facetList.forceActiveFocus(Qt.PopupFocusReason)

    // The row of an option: a check box that shows whether the draft holds the entry, and the tint of the row the keys act on while its list has the focus (the style draws the hover; the check box alone shows what is checked). A click on the row or on the box asks the list to make it current and switch it
    component OptionRow: Controls.ItemDelegate {
        id: option
        objectName: "filterOption_" + entry.key

        // The entry the row stands for, from the model of the list
        required property var entry
        // Whether this is the current row of a list that has the focus
        required property bool current
        readonly property bool selected: dialog.isSelected(entry)
        // A value that would make the query too long for the server
        readonly property bool blocked: !selected && !dialog.canAdd(entry)
        readonly property string hint: blocked ? i18nc("@info hint on a filter value that cannot be added", "Too many filters selected: remove one to add this") : ""

        signal toggleRequested

        enabled: !blocked
        focusPolicy: Qt.NoFocus
        onClicked: toggleRequested()

        // The label is server text for genres, tags and ratings: the control has no text of its own, so no mnemonic or markup applies
        Accessible.role: Accessible.CheckBox
        Accessible.name: entry.label
        Accessible.checkable: true
        Accessible.checked: selected
        Accessible.description: hint

        // Between the background of the style and the content, inside the insets and the radius of the style's own hover
        Rectangle {
            objectName: "filterRowTint"
            anchors.fill: parent
            anchors.leftMargin: option.leftInset
            anchors.rightMargin: option.rightInset
            anchors.topMargin: option.topInset
            anchors.bottomMargin: option.bottomInset
            z: -0.5
            radius: Kirigami.Units.cornerRadius
            visible: option.current
            color: Qt.alpha(Kirigami.Theme.highlightColor, 0.2)
            border.width: 1
            border.color: Qt.alpha(Kirigami.Theme.highlightColor, 0.7)
        }

        PlainToolTip {
            text: option.entry.label
            visible: option.hovered && optionLabel.truncated
        }

        contentItem: RowLayout {
            spacing: Kirigami.Units.largeSpacing

            Controls.CheckBox {
                focusPolicy: Qt.NoFocus
                checked: option.selected
                Accessible.ignored: true
                // Only a user click edits the draft: a changed selection also updates `checked`, which emits toggled and must not invert the selection again
                onClicked: option.toggleRequested()
            }

            Controls.Label {
                id: optionLabel

                Layout.fillWidth: true
                elide: Text.ElideRight
                textFormat: Text.PlainText
                text: option.entry.label
            }

            Controls.Label {
                visible: option.blocked
                Layout.maximumWidth: option.width / 2
                elide: Text.ElideRight
                opacity: 0.7
                textFormat: Text.PlainText
                text: option.hint
            }
        }
    }

    // The entry of the facet list: the title, how many values of the facet the draft checks, and a busy or warning icon while its source loads or has failed. The stock `checked` look says which pane shows
    component FacetRow: Delegates.RoundedItemDelegate {
        id: facetRow

        required property var modelData
        required property int index
        readonly property int count: dialog.countOf(modelData.key)
        readonly property bool busy: modelData.busy ?? false
        readonly property bool failed: modelData.failed ?? false

        objectName: "filterFacet_" + modelData.key
        focusPolicy: Qt.NoFocus
        text: modelData.title
        icon.name: modelData.icon
        checked: !dialog.narrow && dialog.currentFacet === modelData.key
        highlighted: checked
        onClicked: dialog.openFacet(modelData.key)

        Accessible.name: modelData.title
        Accessible.description: busy ? i18n("Loading…") : failed ? i18n("Could not load these filters") : count > 0 ? i18ncp("@item:inlistbox how many values are checked in a facet of the filter dialog", "%1 selected", "%1 selected", count) : ""

        contentItem: RowLayout {
            spacing: Kirigami.Units.smallSpacing

            Delegates.DefaultContentItem {
                itemDelegate: facetRow
                Layout.fillWidth: true
            }

            Controls.Label {
                objectName: "filterFacetCount"
                visible: facetRow.count > 0 && !facetRow.busy && !facetRow.failed
                opacity: 0.7
                text: facetRow.count
                Accessible.ignored: true
            }

            Controls.BusyIndicator {
                visible: facetRow.busy
                running: visible
                Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
                Accessible.ignored: true
            }

            Kirigami.Icon {
                visible: facetRow.failed
                source: "dialog-warning-symbolic"
                Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
                Accessible.ignored: true
            }
        }
    }

    // A list of the dialog: Plasma's wheel step, through the handler the pages use (its key navigation stays off: the keys of the lists are the dialog's own), and the room of the vertical scroll bar stays free so that neither a row nor its tint reaches the bar
    component DialogList: ListView {
        id: view

        // The room of the bar, on the end side of a row: anchors mirror with the layout, a plain x does not (a mirrored delegate sits at x 0 whatever is set)
        readonly property real barRoom: bar.size < 1 ? bar.implicitWidth : 0

        clip: true
        boundsBehavior: Flickable.StopAtBounds
        activeFocusOnTab: true
        keyNavigationEnabled: false
        model: []
        Accessible.role: Accessible.List

        Controls.ScrollBar.vertical: Controls.ScrollBar {
            id: bar
            objectName: view.objectName + "ScrollBar"
        }

        Kirigami.WheelHandler {
            objectName: view.objectName + "Wheel"
            target: view
            filterMouseEvents: true
            keyNavigationEnabled: false
        }
    }

    // Escape is handled here: the focus items below claim it first when it is theirs
    Item {
        id: body

        implicitWidth: Kirigami.Units.gridUnit * 20
        implicitHeight: dialog.bodyHeight

        Keys.onEscapePressed: event => dialog.handleEscape(event)

        RowLayout {
            anchors.fill: parent
            spacing: 0

            // Rows only while the dialog shows
            DialogList {
                id: facetList
                objectName: "filterFacetList"

                Layout.fillHeight: true
                Layout.fillWidth: dialog.narrow
                Layout.preferredWidth: Kirigami.Units.gridUnit * 10
                visible: !dialog.narrow || !dialog.facetOpen
                currentIndex: dialog.facetIndex
                model: dialog.built ? dialog.facets : []
                Accessible.name: i18nc("@info accessible name of the list of filter categories", "Filter categories")

                Keys.onShortcutOverride: event => dialog.keepEscape(event)
                Keys.onUpPressed: dialog.moveFacet(-1)
                Keys.onDownPressed: dialog.moveFacet(1)
                Keys.onReturnPressed: dialog.apply()
                Keys.onEnterPressed: dialog.apply()
                Keys.onSpacePressed: dialog.enterPane()
                // Right goes into the pane, as Tab does (Left in a right-to-left layout)
                Keys.onRightPressed: if (!dialog.rtl) dialog.enterPane()
                Keys.onLeftPressed: if (dialog.rtl) dialog.enterPane()

                delegate: FacetRow {
                    anchors.left: parent?.left
                    anchors.right: parent?.right
                    anchors.rightMargin: facetList.barRoom
                }
            }

            Kirigami.Separator {
                Layout.fillHeight: true
                visible: !dialog.narrow
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: !dialog.narrow || dialog.facetOpen
                spacing: 0

                // Narrow windows only: the facet has the room of the dialog, and Back returns to the facet list
                RowLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: Kirigami.Units.smallSpacing
                    Layout.rightMargin: Kirigami.Units.largeSpacing
                    Layout.topMargin: Kirigami.Units.smallSpacing
                    visible: dialog.narrow
                    spacing: Kirigami.Units.smallSpacing

                    Controls.ToolButton {
                        id: backButton
                        objectName: "filterBack"
                        text: i18nc("@action:button", "Back")
                        icon.name: dialog.rtl ? "go-next" : "go-previous"
                        display: Controls.AbstractButton.IconOnly
                        Controls.ToolTip.text: text
                        Controls.ToolTip.visible: hovered || visualFocus
                        Controls.ToolTip.delay: Kirigami.Units.toolTipDelay
                        onClicked: dialog.leavePane()
                        Keys.onShortcutOverride: event => dialog.keepEscape(event)
                    }

                    Kirigami.Heading {
                        Layout.fillWidth: true
                        level: 4
                        Accessible.role: Accessible.Heading
                        text: dialog.facetEntry?.title ?? ""
                    }
                }

                Kirigami.SearchField {
                    id: searchField
                    objectName: "filterSearch"

                    Layout.fillWidth: true
                    Layout.margins: Kirigami.Units.largeSpacing
                    visible: dialog.searchable
                    // The shell owns Ctrl+F: the field's own sequence would make it ambiguous
                    focusSequences: []
                    maximumLength: 128
                    autoAccept: false
                    text: dialog.pane.text
                    Accessible.description: dialog.facetEntry?.title ?? ""

                    onTextChanged: dialog.searchFor(text)
                    Keys.onShortcutOverride: event => dialog.keepEscape(event)
                    // Return applies the dialog from anywhere (the field's own accepted signal also comes from its clear button)
                    Keys.onReturnPressed: dialog.apply()
                    Keys.onEnterPressed: dialog.apply()
                    Keys.onDownPressed: {
                        if (dialog.rows.length > 0)
                            optionsList.forceActiveFocus(Qt.TabFocusReason);
                    }
                }

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    DialogList {
                        id: optionsList
                        objectName: "filterOptionsList"

                        anchors.fill: parent
                        visible: !(dialog.facetEntry?.busy ?? false) && !(dialog.facetEntry?.failed ?? false)
                        currentIndex: 0
                        model: dialog.rows
                        Accessible.name: dialog.facetEntry?.title ?? ""

                        // A page of rows up or down, the current row following
                        function movePage(direction) {
                            const rows = Math.max(1, Math.floor(height / (currentItem?.height ?? Kirigami.Units.gridUnit * 2)) - 1);
                            currentIndex = Math.max(0, Math.min(count - 1, currentIndex + direction * rows));
                            positionViewAtIndex(currentIndex, ListView.Contain);
                        }
                        // A new facet or search starts at the first row
                        onModelChanged: {
                            currentIndex = 0;
                            positionViewAtBeginning();
                        }
                        Keys.onShortcutOverride: event => dialog.keepEscape(event)
                        // The rows are not tab stops: the list is, and it moves over the options and toggles the current one. Up on the first row goes to the search field, Down on the last stays
                        Keys.onUpPressed: {
                            if (currentIndex > 0) {
                                currentIndex--;
                                positionViewAtIndex(currentIndex, ListView.Contain);
                            } else if (searchField.visible) {
                                searchField.forceActiveFocus(Qt.BacktabFocusReason);
                            }
                        }
                        Keys.onDownPressed: {
                            if (currentIndex < count - 1) {
                                currentIndex++;
                                positionViewAtIndex(currentIndex, ListView.Contain);
                            }
                        }
                        // Space switches the option, Return applies the dialog as its default action
                        Keys.onSpacePressed: dialog.toggleCurrentRow()
                        Keys.onReturnPressed: dialog.apply()
                        Keys.onEnterPressed: dialog.apply()
                        // Left goes back to the facet list, as Shift+Tab does (Right in a right-to-left layout)
                        Keys.onLeftPressed: if (!dialog.rtl) dialog.leavePane()
                        Keys.onRightPressed: if (dialog.rtl) dialog.leavePane()
                        // Home, End, Page Up and Page Down walk a long list (Keys has no signal for them); any other printable key goes to the search
                        Keys.onPressed: event => {
                            if (event.key === Qt.Key_Home) {
                                currentIndex = 0;
                                positionViewAtBeginning();
                            } else if (event.key === Qt.Key_End) {
                                currentIndex = Math.max(0, count - 1);
                                positionViewAtEnd();
                            } else if (event.key === Qt.Key_PageUp || event.key === Qt.Key_PageDown) {
                                movePage(event.key === Qt.Key_PageUp ? -1 : 1);
                            } else {
                                dialog.typeIntoSearch(event);
                                return;
                            }
                            event.accepted = true;
                        }

                        delegate: OptionRow {
                            required property var modelData
                            required property int index

                            anchors.left: parent?.left
                            anchors.right: parent?.right
                            anchors.rightMargin: optionsList.barRoom
                            entry: modelData
                            current: ListView.isCurrentItem && optionsList.activeFocus
                            onToggleRequested: {
                                optionsList.currentIndex = index;
                                optionsList.forceActiveFocus(Qt.MouseFocusReason);
                                dialog.toggle(modelData);
                            }
                        }
                    }

                    Kirigami.LoadingPlaceholder {
                        FadeInOnShow {}
                        anchors.centerIn: parent
                        width: parent.width - Kirigami.Units.gridUnit * 2
                        visible: dialog.facetEntry?.busy ?? false
                        text: i18n("Loading…")
                    }

                    Kirigami.PlaceholderMessage {
                        FadeInOnShow {}
                        objectName: "filterNoMatch"
                        anchors.centerIn: parent
                        width: parent.width - Kirigami.Units.gridUnit * 2
                        visible: dialog.noMatch
                        text: dialog.note
                    }

                    Kirigami.PlaceholderMessage {
                        FadeInOnShow {}
                        anchors.centerIn: parent
                        width: parent.width - Kirigami.Units.gridUnit * 2
                        visible: dialog.facetEntry?.failed ?? false
                        icon.name: "dialog-warning"
                        text: i18n("Could not load these filters")

                        Controls.Button {
                            id: retryButton
                            objectName: "filterRetry_" + (dialog.pane.facet === "genresAndTags" ? "genres" : "legacy")
                            Layout.alignment: Qt.AlignHCenter
                            text: i18n("Retry")
                            icon.name: "view-refresh"
                            Keys.onShortcutOverride: event => dialog.keepEscape(event)
                            onClicked: dialog.options.retry()
                        }
                    }
                }

                Controls.Label {
                    objectName: "filterTagsNote"
                    Layout.fillWidth: true
                    Layout.margins: Kirigami.Units.largeSpacing
                    visible: text !== "" && !dialog.noMatch
                    wrapMode: Text.Wrap
                    opacity: 0.7
                    textFormat: Text.PlainText
                    text: dialog.note
                }
            }
        }
    }
}
