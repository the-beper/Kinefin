// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "LibraryItemsModel.h"

#include "api/MediaSourceSelection.h"
#include "models/SearchModel.h"
#include "settings/BrowseStateStore.h"

#include <QJsonArray>
#include <QJsonValue>

#include <algorithm>
#include <functional>

namespace Kinefin {
namespace {
constexpr LibraryScope kScopes[] = {LibraryScope::Movies, LibraryScope::Series,
                                    LibraryScope::Episodes, LibraryScope::Collections};

const QStringList &imageTypes()
{
    static const QStringList types{QStringLiteral("Primary"), QStringLiteral("Banner"),
                                   QStringLiteral("Disc"), QStringLiteral("Logo"),
                                   QStringLiteral("Thumb")};
    return types;
}

// The image type whose tag a list request has to ask for: Primary, Backdrop and Thumb come with every card request
QString extraImageType(const QString &imageType)
{
    return imageType == QLatin1String("Banner") || imageType == QLatin1String("Disc")
                   || imageType == QLatin1String("Logo")
               ? imageType
               : QString();
}

// The wire token of each key of the older enum
QString sortByFor(LibraryItemsModel::SortKey key)
{
    switch (key) {
    case LibraryItemsModel::Name:
        return QStringLiteral("SortName");
    case LibraryItemsModel::DateAdded:
        return QStringLiteral("DateCreated");
    case LibraryItemsModel::ReleaseDate:
        return QStringLiteral("PremiereDate");
    case LibraryItemsModel::CommunityRating:
        return QStringLiteral("CommunityRating");
    case LibraryItemsModel::DatePlayed:
        return QStringLiteral("DatePlayed");
    case LibraryItemsModel::Runtime:
        return QStringLiteral("Runtime");
    }
    return QStringLiteral("SortName");
}

// Each value once in the order given, at most `limit` of them, only those `accept` lets through
QStringList cleanedList(const QStringList &values, int limit,
                        const std::function<bool(const QString &)> &accept,
                        Qt::CaseSensitivity sensitivity = Qt::CaseSensitive)
{
    QStringList clean;
    for (const QString &value : values) {
        if (clean.size() >= limit)
            break;
        if (accept(value) && !clean.contains(value, sensitivity))
            clean.append(value);
    }
    return clean;
}

// A SHA-256 digest as BrowseStateStore::hashText writes it
bool isDigest(const QString &text)
{
    return text.size() == 64 && std::all_of(text.cbegin(), text.cend(), [](QChar c) {
               const char16_t u = c.unicode();
               return (u >= u'0' && u <= u'9') || (u >= u'a' && u <= u'f');
           });
}

// `current` with the listed texts whose digest is in `hashes` added, in the order listed
QStringList withResolved(QStringList current, const QStringList &listed, const QStringList &hashes)
{
    for (const QString &text : listed) {
        if (current.size() >= MaxFilterValues)
            break;
        if (!current.contains(text) && hashes.contains(BrowseStateStore::hashText(text)))
            current.append(text);
    }
    return current;
}

// The state with the values a query could not carry dropped: bad ids, texts, tokens and years, repeats, and anything past the limits
LibraryFilterState cleanedState(LibraryFilterState state)
{
    state.genreIds =
        cleanedList(state.genreIds, MaxGenreIds, isValidJellyfinId, Qt::CaseInsensitive);
    state.tags = cleanedList(state.tags, MaxFilterValues, isValidFilterText);
    state.officialRatings = cleanedList(state.officialRatings, MaxFilterValues, isValidFilterText);
    state.videoTypes = cleanedList(state.videoTypes, MaxFilterValues, [](const QString &type) {
        return type == QLatin1String("BluRay") || type == QLatin1String("Dvd");
    });
    state.seriesStatus =
        cleanedList(state.seriesStatus, MaxFilterValues, [](const QString &status) {
            return status == QLatin1String("Continuing") || status == QLatin1String("Ended")
                   || status == QLatin1String("Unreleased");
        });
    QList<int> years;
    for (const int year : std::as_const(state.years)) {
        if (years.size() < MaxFilterValues && year >= 1 && year <= 9999 && !years.contains(year))
            years.append(year);
    }
    state.years = years;
    return state;
}

// Every section set, to ask which of them a scope keeps
LibraryFilterState everySection()
{
    LibraryFilterState state;
    state.played = PlayedState::Played;
    state.resumable = true;
    state.favorites = true;
    state.features = LibraryFeature::Subtitles;
    state.quality = VideoQuality::HD;
    state.is4K = true;
    state.is3D = true;
    state.videoTypes = {QStringLiteral("Dvd")};
    state.seriesStatus = {QStringLiteral("Ended")};
    state.episodeKind = EpisodeKind::Specials;
    return state;
}

struct FeatureToken {
    LibraryFeature feature{};
    const char *token{};
};
constexpr FeatureToken kFeatureTokens[] = {{LibraryFeature::Subtitles, "Subtitles"},
                                           {LibraryFeature::Trailer, "Trailer"},
                                           {LibraryFeature::SpecialFeatures, "SpecialFeatures"},
                                           {LibraryFeature::ThemeSong, "ThemeSong"},
                                           {LibraryFeature::ThemeVideo, "ThemeVideo"}};

QString nameOf(PlayedState played)
{
    switch (played) {
    case PlayedState::Any:
        break;
    case PlayedState::Played:
        return QStringLiteral("Played");
    case PlayedState::Unplayed:
        return QStringLiteral("Unplayed");
    }
    return QStringLiteral("Any");
}

QString nameOf(VideoQuality quality)
{
    switch (quality) {
    case VideoQuality::Any:
        break;
    case VideoQuality::HD:
        return QStringLiteral("HD");
    case VideoQuality::SD:
        return QStringLiteral("SD");
    }
    return QStringLiteral("Any");
}

QString nameOf(EpisodeKind kind)
{
    switch (kind) {
    case EpisodeKind::All:
        break;
    case EpisodeKind::Specials:
        return QStringLiteral("Specials");
    case EpisodeKind::Missing:
        return QStringLiteral("Missing");
    case EpisodeKind::Unaired:
        return QStringLiteral("Unaired");
    }
    return QStringLiteral("All");
}
} // namespace

LibraryItemsModel::LibraryItemsModel(JellyfinClient *client, int channel, QObject *parent)
    : PagedItemsModel(PageSize, parent), m_client(client), m_channel(channel),
      m_filterOptions(client, channel, this)
{
    m_searchDebounce.setSingleShot(true);
    m_searchDebounce.setInterval(SearchModel::DebounceMs);
    connect(&m_searchDebounce, &QTimer::timeout, this, [this] { applySearchTerm(); });
    // The options settle the pending tags and ratings and decide which of the selected values still exist, once their source has answered: a failed source keeps the selection, which the user can still remove
    connect(&m_filterOptions, &FilterOptionsModel::statusChanged, this,
            [this] { onFacetsChanged(); });
    connect(&m_filterOptions, &FilterOptionsModel::legacyStatusChanged, this,
            [this] { onFacetsChanged(); });
    if (m_client) {
        connect(m_client, &JellyfinClient::itemsReady, this, &LibraryItemsModel::onItemsReady);
        connect(m_client, &JellyfinClient::listingFailed, this,
                &LibraryItemsModel::onListingFailed);
        connect(m_client, &JellyfinClient::itemFavoriteUpdated, this,
                [this](const QString &id, bool favorite) {
                    applyFavorite(id, favorite, FavoriteRole);
                });
        // A 401, a disconnect, a server switch and a sign-out all end with this signal
        connect(m_client, &JellyfinClient::credentialsCleared, this, [this] { clear(); });
    }
}

std::optional<ItemQuery> LibraryItemsModel::browseQuery(const QString &collectionType)
{
    ItemQuery query;
    query.recursive = true;
    query.cardImages = true;
    if (collectionType == QLatin1String("movies")) {
        query.includeItemTypes = QStringLiteral("Movie");
    } else if (collectionType == QLatin1String("tvshows")) {
        query.includeItemTypes = QStringLiteral("Series");
        // The season and episode counts of a series card
        query.fields = {QStringLiteral("ChildCount"), QStringLiteral("RecursiveItemCount")};
    } else if (collectionType == QLatin1String("boxsets")) {
        query.includeItemTypes = QStringLiteral("BoxSet");
    } else {
        return std::nullopt;
    }
    return query;
}

std::optional<LibraryScope> LibraryItemsModel::scopeFor(const QString &collectionType)
{
    if (collectionType == QLatin1String("movies"))
        return LibraryScope::Movies;
    if (collectionType == QLatin1String("tvshows"))
        return LibraryScope::Series;
    if (collectionType == QLatin1String("boxsets"))
        return LibraryScope::Collections;
    return std::nullopt;
}

QString LibraryItemsModel::sortByFromStored(const QString &stored)
{
    if (stored == QLatin1String("Name"))
        return QStringLiteral("SortName");
    if (stored == QLatin1String("DateAdded"))
        return QStringLiteral("DateCreated");
    if (stored == QLatin1String("ReleaseDate"))
        return QStringLiteral("PremiereDate");
    for (const LibraryScope scope : kScopes) {
        if (librarySortKeys(scope).contains(stored))
            return stored;
    }
    return QStringLiteral("SortName");
}

QVariantMap LibraryItemsModel::filtersToVariant(const LibraryFilterState &state)
{
    QStringList features;
    for (const FeatureToken &entry : kFeatureTokens) {
        if (state.features.testFlag(entry.feature))
            features.append(QLatin1String(entry.token));
    }
    QVariantList years;
    for (const int year : state.years)
        years.append(year);
    return {{QStringLiteral("played"), nameOf(state.played)},
            {QStringLiteral("resumable"), state.resumable},
            {QStringLiteral("favorites"), state.favorites},
            {QStringLiteral("features"), features},
            {QStringLiteral("quality"), nameOf(state.quality)},
            {QStringLiteral("is4K"), state.is4K},
            {QStringLiteral("is3D"), state.is3D},
            {QStringLiteral("videoTypes"), state.videoTypes},
            {QStringLiteral("genreIds"), state.genreIds},
            {QStringLiteral("tags"), state.tags},
            {QStringLiteral("officialRatings"), state.officialRatings},
            {QStringLiteral("years"), years},
            {QStringLiteral("seriesStatus"), state.seriesStatus},
            {QStringLiteral("episodeKind"), nameOf(state.episodeKind)}};
}

LibraryFilterState LibraryItemsModel::filtersFromVariant(const QVariantMap &values)
{
    LibraryFilterState state;
    const QString played = values.value(QStringLiteral("played")).toString();
    if (played == QLatin1String("Played"))
        state.played = PlayedState::Played;
    else if (played == QLatin1String("Unplayed"))
        state.played = PlayedState::Unplayed;
    state.resumable = values.value(QStringLiteral("resumable")).toBool();
    state.favorites = values.value(QStringLiteral("favorites")).toBool();
    const QStringList features = values.value(QStringLiteral("features")).toStringList();
    for (const FeatureToken &entry : kFeatureTokens) {
        if (features.contains(QLatin1String(entry.token)))
            state.features |= entry.feature;
    }
    const QString quality = values.value(QStringLiteral("quality")).toString();
    if (quality == QLatin1String("HD"))
        state.quality = VideoQuality::HD;
    else if (quality == QLatin1String("SD"))
        state.quality = VideoQuality::SD;
    state.is4K = values.value(QStringLiteral("is4K")).toBool();
    state.is3D = values.value(QStringLiteral("is3D")).toBool();
    state.videoTypes = values.value(QStringLiteral("videoTypes")).toStringList();
    state.genreIds = values.value(QStringLiteral("genreIds")).toStringList();
    state.tags = values.value(QStringLiteral("tags")).toStringList();
    state.officialRatings = values.value(QStringLiteral("officialRatings")).toStringList();
    const QVariantList years = values.value(QStringLiteral("years")).toList();
    for (const QVariant &year : years) {
        bool ok = false;
        const int number = year.toInt(&ok);
        if (ok)
            state.years.append(number);
    }
    state.seriesStatus = values.value(QStringLiteral("seriesStatus")).toStringList();
    const QString kind = values.value(QStringLiteral("episodeKind")).toString();
    if (kind == QLatin1String("Specials"))
        state.episodeKind = EpisodeKind::Specials;
    else if (kind == QLatin1String("Missing"))
        state.episodeKind = EpisodeKind::Missing;
    else if (kind == QLatin1String("Unaired"))
        state.episodeKind = EpisodeKind::Unaired;
    return state;
}

bool LibraryItemsModel::open(const QString &libraryId, const QString &collectionType,
                             const QString &libraryName)
{
    return openScope(libraryId, collectionType, libraryName, nullptr, {});
}

bool LibraryItemsModel::open(const QString &libraryId, const QString &collectionType,
                             const QString &libraryName, const ViewState &initial)
{
    return openScope(libraryId, collectionType, libraryName, &initial, {});
}

bool LibraryItemsModel::open(const QString &libraryId, const QString &collectionType,
                             const QString &libraryName, const ViewState &initial,
                             const Constraint &constraint)
{
    return openScope(libraryId, collectionType, libraryName, &initial, constraint);
}

bool LibraryItemsModel::openScope(const QString &libraryId, const QString &collectionType,
                                  const QString &libraryName, const ViewState *initial,
                                  const Constraint &constraint)
{
    const auto base = browseQuery(collectionType);
    const bool sameLibrary = isOpen() && libraryId == m_libraryId
                             && collectionType == m_collectionType && constraint == m_constraint;
    // The search term stays: the tab keeps it while its library selector changes
    clearLibrary();
    if (!base || (!libraryId.isEmpty() && !isValidJellyfinId(libraryId))
        || !std::all_of(constraint.studioIds.cbegin(), constraint.studioIds.cend(),
                        isValidJellyfinId))
        return false;
    m_base = *base;
    m_base.parentId = libraryId;
    if (!constraint.includeItemTypes.isEmpty()) {
        m_base.includeItemTypes = constraint.includeItemTypes;
        // The season and episode counts are of a series card
        m_base.fields.clear();
    }
    m_base.filters = constraint.filters;
    m_base.studioIds = constraint.studioIds;
    m_constraint = constraint;
    m_libraryId = libraryId;
    m_collectionType = collectionType;
    m_libraryName = libraryName;
    // Another library starts from the default order and filters: a section its scope cannot offer would silently empty the grid
    if (initial)
        applyViewState(*initial);
    else if (!sameLibrary)
        applyViewState(defaultViewState(scope()));
    Q_EMIT libraryChanged();
    restartList();
    m_filterOptions.load(libraryId, m_base.includeItemTypes);
    return true;
}

void LibraryItemsModel::clear()
{
    clearLibrary();
    dropSearchTerm();
}

void LibraryItemsModel::clearLibrary()
{
    const bool hadLibrary = isOpen();
    // The options going away say nothing about the selections of the view that is closing
    m_clearingOptions = true;
    m_filterOptions.clear();
    m_clearingOptions = false;
    m_held = false;
    setQueryTooLong(false);
    clearPaging();
    // The letter belongs to the open view
    assignAlphabet({});
    m_constraint = {};
    m_libraryId.clear();
    m_collectionType.clear();
    m_libraryName.clear();
    if (hadLibrary)
        Q_EMIT libraryChanged();
}

LibraryScope LibraryItemsModel::scope() const
{
    return m_constraint.scope.value_or(scopeFor(m_collectionType).value_or(LibraryScope::Movies));
}

QStringList LibraryItemsModel::sortKeys() const
{
    return isOpen() ? librarySortKeys(scope()) : QStringList();
}

bool LibraryItemsModel::acceptsSortKey(const QString &key) const
{
    if (isOpen())
        return librarySortKeys(scope()).contains(key);
    return std::any_of(std::begin(kScopes), std::end(kScopes),
                       [&key](LibraryScope each) { return librarySortKeys(each).contains(key); });
}

LibraryItemsModel::SortKey LibraryItemsModel::sortKey() const
{
    for (const SortKey key : {Name, DateAdded, ReleaseDate, CommunityRating, DatePlayed, Runtime}) {
        if (sortByFor(key) == m_sortBy)
            return key;
    }
    return Name;
}

LibraryItemsModel::Filter LibraryItemsModel::filter() const
{
    if (m_filters.played == PlayedState::Unplayed)
        return Unplayed;
    if (m_filters.played == PlayedState::Played)
        return Played;
    if (m_filters.resumable)
        return Resumable;
    return m_filters.favorites ? Favorites : All;
}

bool LibraryItemsModel::resumableAvailable() const
{
    return isOpen() && everySection().forScope(scope()).resumable;
}

bool LibraryItemsModel::featuresAvailable() const
{
    return isOpen()
           && everySection().forScope(scope()).features.testFlag(LibraryFeature::Subtitles);
}

bool LibraryItemsModel::videoQualityAvailable() const
{
    return isOpen() && everySection().forScope(scope()).is4K;
}

bool LibraryItemsModel::seriesStatusAvailable() const
{
    return isOpen() && !everySection().forScope(scope()).seriesStatus.isEmpty();
}

bool LibraryItemsModel::episodeKindAvailable() const
{
    return isOpen() && everySection().forScope(scope()).episodeKind != EpisodeKind::All;
}

LibraryItemsModel::ViewState LibraryItemsModel::defaultViewState(LibraryScope scope)
{
    ViewState state;
    state.sortBy = librarySortKeys(scope).constFirst();
    return state;
}

LibraryItemsModel::ViewState LibraryItemsModel::viewState() const
{
    ViewState state;
    state.sortBy = m_sortBy;
    state.descending = m_descending;
    state.filters = m_filters;
    state.viewMode = m_viewMode;
    state.imageType = m_imageType;
    state.showTitle = m_showTitle;
    state.showYear = m_showYear;
    state.cardLayout = m_cardLayout;
    state.pendingTagHashes = m_pendingTagHashes;
    state.pendingRatingHashes = m_pendingRatingHashes;
    return state;
}

LibraryFilterState LibraryItemsModel::cleanedFilters(const LibraryFilterState &state) const
{
    const LibraryFilterState clean = cleanedState(state);
    return isOpen() ? clean.forScope(scope()) : clean;
}

void LibraryItemsModel::applyViewState(const ViewState &state)
{
    // A key the scope does not offer falls back to the scope's own first one
    const QString fallback = isOpen() ? librarySortKeys(scope()).constFirst() : state.sortBy;
    assignSortBy(acceptsSortKey(state.sortBy) ? state.sortBy : fallback);
    if (m_descending != state.descending) {
        m_descending = state.descending;
        Q_EMIT descendingChanged();
    }
    // Filters belong to a library: another one starts with its own or none
    assignFilters(state.filters);
    assignPending(state.pendingTagHashes, state.pendingRatingHashes);
    assignViewMode(state.viewMode == QLatin1String("list") ? state.viewMode
                                                           : QStringLiteral("grid"));
    assignImageType(imageTypes().contains(state.imageType) ? state.imageType
                                                           : QStringLiteral("Primary"));
    if (m_showTitle != state.showTitle) {
        m_showTitle = state.showTitle;
        Q_EMIT showTitleChanged();
    }
    if (m_showYear != state.showYear) {
        m_showYear = state.showYear;
        Q_EMIT showYearChanged();
    }
    if (m_cardLayout != state.cardLayout) {
        m_cardLayout = state.cardLayout;
        Q_EMIT cardLayoutChanged();
    }
}

bool LibraryItemsModel::assignSortBy(const QString &key)
{
    if (key == m_sortBy)
        return false;
    m_sortBy = key;
    Q_EMIT sortByChanged();
    Q_EMIT sortKeyChanged();
    return true;
}

bool LibraryItemsModel::assignFilters(const LibraryFilterState &state)
{
    const LibraryFilterState clean = cleanedFilters(state);
    if (clean == m_filters)
        return false;
    const bool genresChanged = clean.genreIds != m_filters.genreIds;
    m_filters = clean;
    Q_EMIT filtersChanged();
    Q_EMIT filterChanged();
    if (genresChanged)
        Q_EMIT genreIdsChanged();
    refreshFiltered();
    return true;
}

bool LibraryItemsModel::assignPending(const QStringList &tagHashes, const QStringList &ratingHashes)
{
    const QStringList tags = cleanedList(tagHashes, MaxFilterValues, isDigest);
    const QStringList ratings = cleanedList(ratingHashes, MaxFilterValues, isDigest);
    if (tags == m_pendingTagHashes && ratings == m_pendingRatingHashes)
        return false;
    const bool wasPending = hasPendingSelections();
    m_pendingTagHashes = tags;
    m_pendingRatingHashes = ratings;
    refreshFiltered();
    if (wasPending != hasPendingSelections())
        Q_EMIT resolvingFacetsChanged();
    return true;
}

bool LibraryItemsModel::assignAlphabet(const QString &letter)
{
    if (letter == m_alphabet)
        return false;
    m_alphabet = letter;
    Q_EMIT alphabetChanged();
    refreshFiltered();
    return true;
}

bool LibraryItemsModel::assignViewMode(const QString &mode)
{
    if (mode == m_viewMode)
        return false;
    m_viewMode = mode;
    Q_EMIT viewModeChanged();
    return true;
}

bool LibraryItemsModel::assignImageType(const QString &type)
{
    if (type == m_imageType)
        return false;
    m_imageType = type;
    Q_EMIT imageTypeChanged();
    return true;
}

void LibraryItemsModel::refreshFiltered()
{
    const bool now = filtered();
    if (now == m_wasFiltered)
        return;
    m_wasFiltered = now;
    Q_EMIT filteredChanged();
}

void LibraryItemsModel::restartList()
{
    m_held = hasPendingSelections();
    if (m_held) {
        setQueryTooLong(false);
        holdPaging(heldStatus());
    } else {
        restartPaging();
    }
}

PagedItemsModel::Status LibraryItemsModel::heldStatus() const
{
    const bool tagsFailed =
        !m_pendingTagHashes.isEmpty() && m_filterOptions.status() == FilterOptionsModel::Failed;
    const bool ratingsFailed = !m_pendingRatingHashes.isEmpty()
                               && m_filterOptions.legacyStatus() == FilterOptionsModel::Failed;
    return tagsFailed || ratingsFailed ? Failed : Loading;
}

void LibraryItemsModel::retry()
{
    // The same query would be refused again
    if (m_queryTooLong)
        return;
    if (m_held)
        m_filterOptions.retry();
    else
        PagedItemsModel::retry();
}

void LibraryItemsModel::resolvePendingSelections()
{
    LibraryFilterState state = m_filters;
    QStringList tagHashes = m_pendingTagHashes;
    QStringList ratingHashes = m_pendingRatingHashes;
    if (!tagHashes.isEmpty() && m_filterOptions.status() == FilterOptionsModel::Ready) {
        state.tags = withResolved(state.tags, m_filterOptions.tags(), tagHashes);
        tagHashes.clear();
    }
    if (!ratingHashes.isEmpty()) {
        const FilterOptionsModel::Status legacy = m_filterOptions.legacyStatus();
        if (legacy == FilterOptionsModel::Ready)
            state.officialRatings = withResolved(state.officialRatings,
                                                 m_filterOptions.officialRatings(), ratingHashes);
        // A server without the legacy endpoint cannot name the saved ratings, and waiting for it would block the view for good: the selection is unavailable, as one the server no longer lists
        if (legacy == FilterOptionsModel::Ready || legacy == FilterOptionsModel::Unavailable)
            ratingHashes.clear();
    }
    // Quietly: a hash no listed text matches is a value the server renamed, and nothing here is the user's edit
    assignFilters(state);
    assignPending(tagHashes, ratingHashes);
}

void LibraryItemsModel::onFacetsChanged()
{
    if (m_clearingOptions || !isOpen())
        return;
    resolvePendingSelections();
    pruneUnavailableSelections();
    if (hasPendingSelections()) {
        if (!m_held || status() != heldStatus())
            restartList();
    } else if (m_held) {
        restartList();
    }
}

void LibraryItemsModel::pruneUnavailableSelections()
{
    LibraryFilterState state = m_filters;
    const auto keepListed = [](const QStringList &selected, const QStringList &listed) {
        QStringList kept;
        for (const QString &value : selected) {
            if (listed.contains(value))
                kept.append(value);
        }
        return kept;
    };
    if (m_filterOptions.status() == FilterOptionsModel::Ready) {
        QStringList kept;
        for (const QString &id : std::as_const(state.genreIds)) {
            if (m_filterOptions.contains(id))
                kept.append(id);
        }
        state.genreIds = kept;
        state.tags = keepListed(state.tags, m_filterOptions.tags());
    }
    if (m_filterOptions.legacyStatus() == FilterOptionsModel::Ready) {
        state.officialRatings =
            keepListed(state.officialRatings, m_filterOptions.officialRatings());
        const QList<int> listed = m_filterOptions.years();
        state.years.removeIf([&listed](int year) { return !listed.contains(year); });
    }
    // A prune is not an edit of the user: the view state is not reported
    if (state != m_filters && assignFilters(state) && isOpen())
        restartList();
}

void LibraryItemsModel::setSortBy(const QString &key)
{
    if (key == m_sortBy || !acceptsSortKey(key))
        return;
    assignSortBy(key);
    if (isRandomSort(key))
        assignAlphabet({});
    if (isOpen())
        restartList();
    Q_EMIT viewStateEdited();
}

void LibraryItemsModel::setSortKey(SortKey key)
{
    setSortBy(sortByFor(key));
}

void LibraryItemsModel::setDescending(bool descending)
{
    if (descending == m_descending)
        return;
    m_descending = descending;
    Q_EMIT descendingChanged();
    // The direction of a random order means nothing
    if (isOpen() && !isRandom())
        restartList();
    Q_EMIT viewStateEdited();
}

void LibraryItemsModel::setFilterState(const LibraryFilterState &state)
{
    const LibraryFilterState clean = cleanedFilters(state);
    const int chars = queryCharsOf(clean);
    if (chars > MaxItemsQueryChars && chars >= queryCharsOf(m_filters))
        return;
    if (!assignFilters(state))
        return;
    if (isOpen())
        restartList();
    Q_EMIT viewStateEdited();
}

void LibraryItemsModel::setFilterValues(const QVariantMap &values)
{
    setFilterState(filtersFromVariant(values));
}

bool LibraryItemsModel::filtersFit(const QVariantMap &filters) const
{
    return queryCharsOf(cleanedFilters(filtersFromVariant(filters))) <= MaxItemsQueryChars;
}

void LibraryItemsModel::setFilter(Filter filter)
{
    LibraryFilterState state = m_filters;
    state.played = filter == Unplayed ? PlayedState::Unplayed
                   : filter == Played ? PlayedState::Played
                                      : PlayedState::Any;
    state.resumable = filter == Resumable;
    state.favorites = filter == Favorites;
    setFilterState(state);
}

void LibraryItemsModel::setGenreIds(const QStringList &ids)
{
    LibraryFilterState state = m_filters;
    state.genreIds = ids;
    setFilterState(state);
}

void LibraryItemsModel::clearFilters()
{
    const bool droppedPending = assignPending({}, {});
    if (!assignFilters({}) && !droppedPending)
        return;
    if (isOpen())
        restartList();
    Q_EMIT viewStateEdited();
}

void LibraryItemsModel::setAlphabet(const QString &letter)
{
    QString clean = letter;
    if (clean.size() == 1 && clean.at(0) >= QLatin1Char('a') && clean.at(0) <= QLatin1Char('z'))
        clean = clean.toUpper();
    const bool valid = clean.isEmpty() || clean == QLatin1String("#")
                       || (clean.size() == 1 && clean.at(0) >= QLatin1Char('A')
                           && clean.at(0) <= QLatin1Char('Z'));
    if (!valid || (isRandom() && !clean.isEmpty()))
        return;
    if (assignAlphabet(clean) && isOpen())
        restartList();
}

void LibraryItemsModel::reload()
{
    if (isOpen())
        restartList();
}

void LibraryItemsModel::reshuffle()
{
    if (isOpen() && isRandom())
        restartList();
}

void LibraryItemsModel::setViewMode(const QString &mode)
{
    if (mode != QLatin1String("grid") && mode != QLatin1String("list"))
        return;
    if (assignViewMode(mode))
        Q_EMIT viewStateEdited();
}

void LibraryItemsModel::setImageType(const QString &type)
{
    if (!imageTypes().contains(type))
        return;
    const QString before = extraImageType(m_imageType);
    if (!assignImageType(type))
        return;
    const QString extra = extraImageType(type);
    if (isOpen() && !extra.isEmpty() && extra != before)
        restartList();
    else if (rowCount() > 0)
        Q_EMIT dataChanged(index(0), index(rowCount() - 1), {ImageUrlRole});
    Q_EMIT viewStateEdited();
}

void LibraryItemsModel::setShowTitle(bool show)
{
    if (show == m_showTitle)
        return;
    m_showTitle = show;
    Q_EMIT showTitleChanged();
    Q_EMIT viewStateEdited();
}

void LibraryItemsModel::setShowYear(bool show)
{
    if (show == m_showYear)
        return;
    m_showYear = show;
    Q_EMIT showYearChanged();
    Q_EMIT viewStateEdited();
}

void LibraryItemsModel::setCardLayout(bool layout)
{
    if (layout == m_cardLayout)
        return;
    m_cardLayout = layout;
    Q_EMIT cardLayoutChanged();
    Q_EMIT viewStateEdited();
}

void LibraryItemsModel::setSearchTerm(const QString &term)
{
    if (term == m_searchTerm)
        return;
    m_searchTerm = term;
    Q_EMIT searchTermChanged();
    const QString clean = SearchModel::normalized(term);
    const bool tooShort = clean.size() < SearchModel::MinTermLength;
    // The same words again (a trailing space, a second too-short edit): what is shown or in flight stays
    if ((tooShort ? QString() : clean) == m_appliedTerm) {
        m_searchDebounce.stop();
        return;
    }
    if (tooShort) {
        m_searchDebounce.stop();
        applySearchTerm();
        return;
    }
    m_searchDebounce.start();
}

void LibraryItemsModel::applySearchTerm()
{
    const QString clean = SearchModel::normalized(m_searchTerm);
    const QString applied = clean.size() < SearchModel::MinTermLength ? QString() : clean;
    if (applied == m_appliedTerm)
        return;
    m_appliedTerm = applied;
    refreshFiltered();
    if (isOpen())
        restartList();
}

void LibraryItemsModel::dropSearchTerm()
{
    m_searchDebounce.stop();
    const bool hadTerm = !m_searchTerm.isEmpty();
    m_searchTerm.clear();
    m_appliedTerm.clear();
    if (hadTerm)
        Q_EMIT searchTermChanged();
    refreshFiltered();
}

QHash<int, QByteArray> LibraryItemsModel::roleNames() const
{
    QHash<int, QByteArray> names{{ItemIdRole, "itemId"},
                                 {NameRole, "name"},
                                 {YearRole, "year"},
                                 {TypeRole, "type"},
                                 {ImageUrlRole, "imageUrl"},
                                 {PlayedRole, "played"},
                                 {UnplayedCountRole, "unplayedCount"},
                                 {PlayedPercentageRole, "playedPercentage"},
                                 {FavoriteRole, "favorite"},
                                 {LoadedRole, "loaded"},
                                 {SeriesNameRole, "seriesName"},
                                 {SeasonNumberRole, "seasonNumber"},
                                 {EpisodeNumberRole, "episodeNumber"}};
    names.insert(cardRoleNames());
    return names;
}

// The image of the chosen type, or the Primary when the item has none of it
QUrl LibraryItemsModel::cardImage(const ItemSummary &item) const
{
    if (!item.valid || !m_client)
        return {};
    QString type = m_imageType;
    QString tag;
    if (type == QLatin1String("Banner"))
        tag = item.bannerTag;
    else if (type == QLatin1String("Disc"))
        tag = item.discTag;
    else if (type == QLatin1String("Logo"))
        tag = item.logoTag;
    else if (type == QLatin1String("Thumb"))
        tag = item.thumbTag;
    if (tag.isEmpty()) {
        type = QStringLiteral("Primary");
        tag = item.primaryTag;
    }
    return tag.isEmpty() ? QUrl() : m_client->imageUrl(item.id, type, ImageWidth, tag);
}

QVariant LibraryItemsModel::rowData(const ItemSummary &item, bool loaded, int role) const
{
    switch (role) {
    case LoadedRole:
        return loaded;
    case SeriesNameRole:
        return item.type == QLatin1String("Episode") ? item.seriesName : QString();
    case SeasonNumberRole:
        return item.seasonNumber;
    case EpisodeNumberRole:
        return item.episodeNumber;
    case Qt::DisplayRole:
    case NameRole:
        return item.name;
    case ItemIdRole:
        return item.id;
    case YearRole:
        return item.year;
    case TypeRole:
        return item.type;
    case ImageUrlRole:
        return cardImage(item);
    case PlayedRole:
        return item.played;
    case UnplayedCountRole:
        return item.unplayedCount;
    case PlayedPercentageRole:
        return item.playedPercentage;
    case FavoriteRole:
        return item.favorite;
    default:
        return cardRoleData(item, role);
    }
}

std::optional<ItemQuery> LibraryItemsModel::composedQuery(const LibraryFilterState &filters,
                                                          int start, int limit) const
{
    ItemQuery base = m_base;
    base.searchTerm = m_appliedTerm;
    base.cardImageType = extraImageType(m_imageType);
    return composeLibraryQuery(base, scope(), filters, LibrarySort{m_sortBy, m_descending},
                               m_alphabet, start, limit);
}

int LibraryItemsModel::queryCharsOf(const LibraryFilterState &filters) const
{
    // The widest start index and page size a request can carry (Random is the one first page)
    const std::optional<ItemQuery> query =
        composedQuery(filters, isRandom() ? 0 : MaxRows, PageSize);
    return query && m_client ? m_client->itemsQueryChars(*query) : -1;
}

void LibraryItemsModel::setQueryTooLong(bool tooLong)
{
    if (tooLong == m_queryTooLong)
        return;
    m_queryTooLong = tooLong;
    Q_EMIT queryTooLongChanged();
}

bool LibraryItemsModel::sendPage(int start, int limit)
{
    // A pending tag or rating is part of the saved view: sending without it would list more than the user kept
    if (hasPendingSelections())
        return false;
    const std::optional<ItemQuery> query = composedQuery(m_filters, start, limit);
    if (!query || !m_client)
        return false;
    // The client would refuse it too: say why instead of failing without a word
    setQueryTooLong(m_client->itemsQueryChars(*query) > MaxItemsQueryChars);
    return !m_queryTooLong && m_client->fetchItems(*query, m_channel);
}

void LibraryItemsModel::onItemsReady(const QJsonArray &items, int total, int channel)
{
    if (channel != m_channel || !pageInFlight() || !isOpen())
        return;
    // A random order has the one page: a larger total would ask for pages that can never be the same set
    deliverPage(items, isRandom() ? std::min(total, RandomPageSize) : total);
}

void LibraryItemsModel::onListingFailed(const QString &family, int, const QString &, int channel)
{
    if (family != QLatin1String("Items") || channel != m_channel || !pageInFlight() || !isOpen())
        return;
    failPageInFlight();
}
} // namespace Kinefin
