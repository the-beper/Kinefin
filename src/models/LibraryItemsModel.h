// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/JellyfinClient.h"
#include "models/CardRoles.h"
#include "models/FilterOptionsModel.h"
#include "models/LibraryFilterState.h"
#include "models/PagedItemsModel.h"

#include <QTimer>
#include <QUrl>
#include <QVariantMap>

#include <optional>

namespace Kinefin {
// The items of one library, or of every library of a type, as a paged list (see PagedItemsModel, with the card roles of CardRoles.h): the query of the type with a sort, the typed filters of LibraryFilterState and a search term, on a listing channel given at construction (the filter options use the same channel number in their own families) so the home rows, the other grids and the details pages never see its replies. Every request goes through composeLibraryQuery, so each active section is sent, the sections the scope does not offer are left out, and an invalid combination is refused without a request. The model empties itself whenever the client loses its credentials. A tag or official rating kept by an earlier session is known by its SHA-256 digest only (see `ViewState::pendingTagHashes`): such a selection is pending, the first request of the view waits for the facet list that can name it (the status is Loading, or Failed with `retry()` asking that list again), and a request less restrictive than the saved view is never sent
class LibraryItemsModel final : public PagedItemsModel {
    Q_OBJECT
    // The `sortBy` token of the order, one of `sortKeys` (the keys of the open scope; Random is one page of at most RandomPageSize items, reshuffle() asks for another)
    Q_PROPERTY(QString sortBy READ sortBy WRITE setSortBy NOTIFY sortByChanged)
    Q_PROPERTY(QStringList sortKeys READ sortKeys NOTIFY libraryChanged)
    Q_PROPERTY(bool random READ isRandom NOTIFY sortByChanged)
    // The older name of the first six keys, for the callers that still use the enum
    Q_PROPERTY(SortKey sortKey READ sortKey WRITE setSortKey NOTIFY sortKeyChanged)
    Q_PROPERTY(bool descending READ descending WRITE setDescending NOTIFY descendingChanged)
    // The filters as a map for QML (see filtersToVariant): the keys are those of LibraryFilterState, the values tokens and plain lists. Writing a map replaces every section, a missing key is the section's default
    Q_PROPERTY(QVariantMap filters READ filterValues WRITE setFilterValues NOTIFY filtersChanged)
    // The older single status choice, a view of the status sections of `filters`
    Q_PROPERTY(Filter filter READ filter WRITE setFilter NOTIFY filterChanged)
    // Which sections of the filter dialog the open scope offers
    Q_PROPERTY(bool resumableAvailable READ resumableAvailable NOTIFY libraryChanged)
    Q_PROPERTY(bool featuresAvailable READ featuresAvailable NOTIFY libraryChanged)
    Q_PROPERTY(bool videoQualityAvailable READ videoQualityAvailable NOTIFY libraryChanged)
    Q_PROPERTY(bool seriesStatusAvailable READ seriesStatusAvailable NOTIFY libraryChanged)
    Q_PROPERTY(bool episodeKindAvailable READ episodeKindAvailable NOTIFY libraryChanged)
    Q_PROPERTY(QString libraryName READ libraryName NOTIFY libraryChanged)
    Q_PROPERTY(QString libraryId READ libraryId NOTIFY libraryChanged)
    // The kind of list that saved filters belong to: `movies`, `series`, `episodes` or `collections`; empty while the model is closed
    Q_PROPERTY(QString filterKind READ filterKind NOTIFY libraryChanged)
    // The genre ids the grid is filtered by (any of them), chosen among `filterOptions`
    Q_PROPERTY(QStringList genreIds READ genreIds WRITE setGenreIds NOTIFY genreIdsChanged)
    // The values the filters can take: the genres, tags, official ratings and years of the open scope
    Q_PROPERTY(Kinefin::FilterOptionsModel *filterOptions READ filterOptions CONSTANT)
    // How the items are shown: `viewMode` "grid" or "list", `imageType` one of Primary, Banner, Disc, Logo and Thumb (an item without that image shows its Primary), and the presentation switches of the cards. None of them changes the items, so none reloads the list but a new image type whose tags were not asked for
    Q_PROPERTY(QString viewMode READ viewMode WRITE setViewMode NOTIFY viewModeChanged)
    Q_PROPERTY(QString imageType READ imageType WRITE setImageType NOTIFY imageTypeChanged)
    Q_PROPERTY(bool showTitle READ showTitle WRITE setShowTitle NOTIFY showTitleChanged)
    Q_PROPERTY(bool showYear READ showYear WRITE setShowYear NOTIFY showYearChanged)
    Q_PROPERTY(bool cardLayout READ cardLayout WRITE setCardLayout NOTIFY cardLayoutChanged)
    // What the user typed in the search box: a pause of `SearchModel::DebounceMs` after the last edit reloads the list with the normalised term, a term that becomes too short reloads at once, and the same normalised term sends nothing. Kept across open(), dropped by clear()
    Q_PROPERTY(QString searchTerm READ searchTerm WRITE setSearchTerm NOTIFY searchTermChanged)
    // Whether any filter (a pending tag or rating included) or search term narrows the list (the empty-state text depends on it)
    Q_PROPERTY(bool filtered READ filtered NOTIFY filteredChanged)
    // Whether a saved tag or rating still waits for its facet list: the list is held back (Loading, or Failed with retry()) until it is resolved
    Q_PROPERTY(bool resolvingFacets READ resolvingFacets NOTIFY resolvingFacetsChanged)
    // Whether the last request was refused because the query is longer than MaxItemsQueryChars (a search term or a saved view past the budget the filter editor keeps): the list is Failed and retry() does nothing, only a shorter query (clearFilters(), a shorter search term) helps
    Q_PROPERTY(bool queryTooLong READ queryTooLong NOTIFY queryTooLongChanged)

  public:
    enum SortKey { Name, DateAdded, ReleaseDate, CommunityRating, DatePlayed, Runtime };
    Q_ENUM(SortKey)
    enum Filter { All, Unplayed, Played, Resumable, Favorites };
    Q_ENUM(Filter)
    enum Role {
        ItemIdRole = Qt::UserRole + 1,
        NameRole,
        YearRole,
        TypeRole,
        ImageUrlRole,
        PlayedRole,
        UnplayedCountRole,
        PlayedPercentageRole,
        FavoriteRole,
        LoadedRole,
        // The series of an episode (empty for anything else) and the season and episode numbers (-1 when the item has none)
        SeriesNameRole,
        SeasonNumberRole,
        EpisodeNumberRole
    };

    // The order, filters and view options of a view: what the browse state store keeps per scope. `sortBy` is a wire token of librarySortKeys(); `viewMode` is "grid" or "list"; `imageType` is Primary, Banner, Disc, Logo or Thumb. A value the model does not accept is replaced by the default when applied. Two equal states are the same view
    struct ViewState {
        QString sortBy = QStringLiteral("SortName");
        bool descending = false;
        LibraryFilterState filters;
        QString viewMode = QStringLiteral("grid");
        QString imageType = QStringLiteral("Primary");
        bool showTitle = true;
        bool showYear = true;
        bool cardLayout = true;
        // The `BrowseStateStore::hashText` digests of tags and official ratings saved by an earlier session, whose texts are not known yet: the model resolves them into `filters.tags` and `filters.officialRatings` once the facet list that holds the texts has loaded (the tags of Filters2, the ratings of the legacy filters), drops the ones no listed text matches, and reports what is still unresolved here
        QStringList pendingTagHashes;
        QStringList pendingRatingHashes;
        bool operator==(const ViewState &) const = default;
    };

    // What a secondary list fixes besides the type's own query, none of it editable by the user: `scope` (when the list is not of the type's own scope, such as the episodes of a TV library), `includeItemTypes` (replaces the type's), `filters` (ItemFilter tokens such as the Favorites list's IsFavorite, which clearFilters() leaves alone) and `studioIds` (the series of a network)
    struct Constraint {
        std::optional<LibraryScope> scope;
        QString includeItemTypes;
        QString filters;
        QStringList studioIds;
        bool operator==(const Constraint &) const = default;
    };

    static constexpr int PageSize = 100;
    // The listing channels of the grids: the tile-opened library (collections), the Movies tab and the TV Shows tab
    static constexpr int TileChannel = 1;
    static constexpr int MoviesChannel = 6;
    static constexpr int ShowsChannel = 7;
    // The width requested for a poster: the tiles show at most about this many device pixels
    static constexpr int ImageWidth = 400;

    LibraryItemsModel(JellyfinClient *client, int channel, QObject *parent = nullptr);

    // The query that lists the items of a library of this `CollectionType`, or nothing when the type is not browsable (movies, tvshows and boxsets are)
    static std::optional<ItemQuery> browseQuery(const QString &collectionType);
    // The scope of a browsable `CollectionType`: what decides the filter sections and the sort keys
    static std::optional<LibraryScope> scopeFor(const QString &collectionType);
    // The `sortBy` token of a stored sort key: the tokens of librarySortKeys() as they are, the older names (Name, DateAdded, ReleaseDate) mapped, anything else the default SortName
    static QString sortByFromStored(const QString &stored);
    // The filters as the map of the `filters` property and back. Tokens: `played` Any, Played or Unplayed; `quality` Any, HD or SD; `episodeKind` All, Specials, Missing or Unaired; `features` a list of Subtitles, Trailer, SpecialFeatures, ThemeSong and ThemeVideo; `resumable`, `favorites`, `is4K`, `is3D` booleans; `videoTypes`, `genreIds`, `tags`, `officialRatings`, `seriesStatus` lists of texts; `years` a list of numbers. Unknown keys and tokens are ignored
    static QVariantMap filtersToVariant(const LibraryFilterState &state);
    static LibraryFilterState filtersFromVariant(const QVariantMap &values);

    // The view a scope starts from when nothing was stored: the default state with the first sort key of the scope (the aired order for episodes)
    static ViewState defaultViewState(LibraryScope scope);

    // Resets the model and loads the first page. An empty `libraryId` lists every library of the type (no parent on the wire). False (and the model stays empty) for a library id the server could not have sent or a type that is not browsable
    bool open(const QString &libraryId, const QString &collectionType,
              const QString &libraryName = QString());
    // The same, starting from `initial` instead of the default order and filters (applied without viewStateEdited). The filter sections and the sort key the scope does not offer are dropped
    bool open(const QString &libraryId, const QString &collectionType, const QString &libraryName,
              const ViewState &initial);
    // The same with a fixed part of the query: a list of this model that is not the plain library grid (Favorites, the episodes of a library, a network's series). An invalid studio id refuses it like a bad library id does
    bool open(const QString &libraryId, const QString &collectionType, const QString &libraryName,
              const ViewState &initial, const Constraint &constraint);
    // Empties the model and drops the search term
    void clear();
    // Whether a library (or the whole type) is open
    bool isOpen() const { return !m_collectionType.isEmpty(); }

    QString sortBy() const { return m_sortBy; }
    QStringList sortKeys() const;
    bool isRandom() const { return isRandomSort(m_sortBy); }
    SortKey sortKey() const;
    bool descending() const { return m_descending; }
    LibraryFilterState filterState() const { return m_filters; }
    QVariantMap filterValues() const { return filtersToVariant(m_filters); }
    Filter filter() const;
    bool resumableAvailable() const;
    bool featuresAvailable() const;
    bool videoQualityAvailable() const;
    bool seriesStatusAvailable() const;
    bool episodeKindAvailable() const;
    QString libraryName() const { return m_libraryName; }
    QString libraryId() const { return m_libraryId; }
    QString collectionType() const { return m_collectionType; }
    QStringList genreIds() const { return m_filters.genreIds; }
    FilterOptionsModel *filterOptions() { return &m_filterOptions; }
    FilterOptionsModel *genreOptions() { return &m_filterOptions; }
    QString viewMode() const { return m_viewMode; }
    QString imageType() const { return m_imageType; }
    bool showTitle() const { return m_showTitle; }
    bool showYear() const { return m_showYear; }
    bool cardLayout() const { return m_cardLayout; }
    bool filtered() const
    {
        return !m_filters.isEmpty() || hasPendingSelections() || !m_appliedTerm.isEmpty();
    }
    bool resolvingFacets() const { return hasPendingSelections(); }
    bool queryTooLong() const { return m_queryTooLong; }
    QStringList pendingTagHashes() const { return m_pendingTagHashes; }
    QStringList pendingRatingHashes() const { return m_pendingRatingHashes; }
    ViewState viewState() const;
    QString searchTerm() const { return m_searchTerm; }
    void setSearchTerm(const QString &term);
    // For tests; the default is SearchModel::DebounceMs
    void setSearchDebounceInterval(int milliseconds) { m_searchDebounce.setInterval(milliseconds); }

    // The setters below are the user's edits: each one that changes something reloads the list when it matters (not the view options) and emits viewStateEdited, an invalid value changes nothing. Here a key the open scope does not offer is refused
    void setSortBy(const QString &key);
    void setSortKey(SortKey key);
    void setDescending(bool descending);
    // Replaces every filter section; values the query could not carry are dropped, and so are the sections the scope does not offer. A state whose query would pass MaxItemsQueryChars is refused (nothing changes) unless it is shorter than the current one, so a view that is already over can always be reduced
    void setFilterState(const LibraryFilterState &state);
    void setFilterValues(const QVariantMap &values);
    // Whether the query of the open view with `filters` (the format of the `filters` property), with the sort and search term of now, stays within MaxItemsQueryChars: what the filter editor asks before it lets a value in
    Q_INVOKABLE bool filtersFit(const QVariantMap &filters) const;
    void setFilter(Filter filter);
    void setGenreIds(const QStringList &ids);
    void setViewMode(const QString &mode);
    void setImageType(const QString &type);
    void setShowTitle(bool show);
    void setShowYear(bool show);
    void setCardLayout(bool layout);
    // Clears every filter section, the pending tags and ratings included (the view then no longer waits for a facet list), not the sort or the search term
    Q_INVOKABLE void clearFilters();
    // Replaces every filter section and drops the pending tags and ratings in the same step: for a caller that starts from another view, like a genre rail's Show All
    Q_INVOKABLE void replaceFilters(const QVariantMap &filters);
    // Replaces every filter section by a saved set, whose tags and ratings are known by digest only: they stay pending until the facet lists that hold their texts load, as in a view that an earlier session saved. The sort, the search and the view options stay
    void applyFilterSet(const LibraryFilterState &filters, const QStringList &pendingTagHashes,
                        const QStringList &pendingRatingHashes);
    QString filterKind() const;
    // Held back while a saved tag or rating is pending: asks the failed facet source again instead of the list (nothing is sent before it resolves)
    void retry() override;
    // Asks for the list again from its first page, keeping the query and the view; nothing while it is closed
    Q_INVOKABLE void reload();
    // With Random sorting, asks for a new page of random items; the filters stay. Nothing otherwise
    Q_INVOKABLE void reshuffle();

    QHash<int, QByteArray> roleNames() const override;

  Q_SIGNALS:
    void sortByChanged();
    void sortKeyChanged();
    void descendingChanged();
    void filtersChanged();
    void filterChanged();
    void libraryChanged();
    void genreIdsChanged();
    void filteredChanged();
    void viewModeChanged();
    void imageTypeChanged();
    void showTitleChanged();
    void showYearChanged();
    void cardLayoutChanged();
    void resolvingFacetsChanged();
    void searchTermChanged();
    void queryTooLongChanged();
    // The user changed the sort, direction, filters or view options (not an open(), a resolution or prune of values the server no longer lists or a search term)
    void viewStateEdited();

  protected:
    bool sendPage(int start, int limit) override;
    QVariant rowData(const ItemSummary &item, bool loaded, int role) const override;

  private:
    bool openScope(const QString &libraryId, const QString &collectionType,
                   const QString &libraryName, const ViewState *initial,
                   const Constraint &constraint);
    void clearLibrary();
    void applyViewState(const ViewState &state);
    LibraryScope scope() const;
    bool acceptsSortKey(const QString &key) const;
    LibraryFilterState cleanedFilters(const LibraryFilterState &state) const;
    bool assignSortBy(const QString &key);
    bool assignFilters(const LibraryFilterState &state);
    bool assignPending(const QStringList &tagHashes, const QStringList &ratingHashes);
    bool hasPendingSelections() const
    {
        return !m_pendingTagHashes.isEmpty() || !m_pendingRatingHashes.isEmpty();
    }
    // Reloads the list from the first page, or holds it back while a selection is pending
    void restartList();
    PagedItemsModel::Status heldStatus() const;
    // The pending selections the loaded facet lists can settle: resolved into texts, or dropped when no listed text matches (or the legacy endpoint is missing). Not an edit
    void resolvePendingSelections();
    void onFacetsChanged();
    // The query of one page of the open view with `filters` (already cleaned), or nothing for a sort key the composer refuses
    std::optional<ItemQuery> composedQuery(const LibraryFilterState &filters, int start,
                                           int limit) const;
    // The encoded length of the longest page request of the view with `filters`, -1 when there is no query
    int queryCharsOf(const LibraryFilterState &filters) const;
    void setQueryTooLong(bool tooLong);
    bool assignViewMode(const QString &mode);
    bool assignImageType(const QString &type);
    void refreshFiltered();
    void pruneUnavailableSelections();
    QUrl cardImage(const ItemSummary &item) const;
    void applySearchTerm();
    void dropSearchTerm();
    void onItemsReady(const QJsonArray &items, int total, int channel);
    void onListingFailed(const QString &family, int status, const QString &reason, int channel);

    JellyfinClient *m_client;
    const int m_channel;
    ItemQuery m_base;
    Constraint m_constraint;
    QString m_libraryId;
    QString m_collectionType;
    QString m_libraryName;
    QString m_sortBy = QStringLiteral("SortName");
    bool m_descending = false;
    LibraryFilterState m_filters;
    // Digests of the saved tags and ratings still waiting for their facet list
    QStringList m_pendingTagHashes;
    QStringList m_pendingRatingHashes;
    // The list is held back for the pending selections: nothing was asked for
    bool m_held = false;
    // clearLibrary is emptying the facet options: the selections of the closing view are not to be settled by it
    bool m_clearingOptions = false;
    QString m_viewMode = QStringLiteral("grid");
    QString m_imageType = QStringLiteral("Primary");
    bool m_showTitle = true;
    bool m_showYear = true;
    bool m_cardLayout = true;
    // The value `filtered` last had, for its change signal
    bool m_wasFiltered = false;
    bool m_queryTooLong = false;
    // The typed text, and the normalised term the list was last loaded with (empty when the text is too short)
    QString m_searchTerm;
    QString m_appliedTerm;
    QTimer m_searchDebounce;
    FilterOptionsModel m_filterOptions;
};
} // namespace Kinefin
