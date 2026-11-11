// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "LibraryItemsModel.h"

#include "api/MediaSourceSelection.h"
#include "models/SearchModel.h"

#include <QJsonArray>
#include <QJsonValue>

#include <algorithm>

namespace Kinefin {
namespace {
// Each key ends in SortName so the order of equal values is stable across pages
QString sortByFor(LibraryItemsModel::SortKey key)
{
    switch (key) {
    case LibraryItemsModel::Name:
        return QStringLiteral("SortName");
    case LibraryItemsModel::DateAdded:
        return QStringLiteral("DateCreated,SortName");
    case LibraryItemsModel::ReleaseDate:
        return QStringLiteral("PremiereDate,SortName");
    case LibraryItemsModel::CommunityRating:
        return QStringLiteral("CommunityRating,SortName");
    case LibraryItemsModel::DatePlayed:
        return QStringLiteral("DatePlayed,SortName");
    case LibraryItemsModel::Runtime:
        return QStringLiteral("Runtime,SortName");
    }
    return QStringLiteral("SortName");
}

QString filtersFor(LibraryItemsModel::Filter filter)
{
    switch (filter) {
    case LibraryItemsModel::All:
        return {};
    case LibraryItemsModel::Unplayed:
        return QStringLiteral("IsUnplayed");
    case LibraryItemsModel::Played:
        return QStringLiteral("IsPlayed");
    case LibraryItemsModel::Resumable:
        return QStringLiteral("IsResumable");
    case LibraryItemsModel::Favorites:
        return QStringLiteral("IsFavorite");
    }
    return {};
}
} // namespace

LibraryItemsModel::LibraryItemsModel(JellyfinClient *client, int channel, QObject *parent)
    : PagedItemsModel(PageSize, parent), m_client(client), m_channel(channel),
      m_genreOptions(client, channel, this)
{
    m_searchDebounce.setSingleShot(true);
    m_searchDebounce.setInterval(SearchModel::DebounceMs);
    connect(&m_searchDebounce, &QTimer::timeout, this, [this] { applySearchTerm(); });
    // The options decide which of the selected genres still exist: the others are dropped, all of them when the options cannot be loaded (the menu that clears them is hidden then)
    connect(&m_genreOptions, &FilterOptionsModel::statusChanged, this, [this] {
        if (m_genreIds.isEmpty())
            return;
        // A prune is not an edit of the user: the view state is not reported
        const auto prune = [this](const QStringList &ids) {
            if (assignGenreIds(ids) && isOpen())
                restartPaging();
        };
        if (m_genreOptions.status() == FilterOptionsModel::Failed) {
            prune({});
            return;
        }
        if (m_genreOptions.status() != FilterOptionsModel::Ready)
            return;
        QStringList kept;
        for (const QString &id : std::as_const(m_genreIds)) {
            if (m_genreOptions.contains(id))
                kept.append(id);
        }
        prune(kept);
    });
    if (m_client) {
        connect(m_client, &JellyfinClient::itemsReady, this, &LibraryItemsModel::onItemsReady);
        connect(m_client, &JellyfinClient::listingFailed, this,
                &LibraryItemsModel::onListingFailed);
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

bool LibraryItemsModel::open(const QString &libraryId, const QString &collectionType,
                             const QString &libraryName)
{
    return openScope(libraryId, collectionType, libraryName, nullptr);
}

bool LibraryItemsModel::open(const QString &libraryId, const QString &collectionType,
                             const QString &libraryName, const ViewState &initial)
{
    return openScope(libraryId, collectionType, libraryName, &initial);
}

bool LibraryItemsModel::openScope(const QString &libraryId, const QString &collectionType,
                                  const QString &libraryName, const ViewState *initial)
{
    const auto base = browseQuery(collectionType);
    const bool sameLibrary =
        isOpen() && libraryId == m_libraryId && collectionType == m_collectionType;
    // The search term stays: the tab keeps it while its library selector changes
    clearLibrary();
    if (!base || (!libraryId.isEmpty() && !isValidJellyfinId(libraryId)))
        return false;
    m_base = *base;
    m_base.parentId = libraryId;
    m_libraryId = libraryId;
    m_collectionType = collectionType;
    m_libraryName = libraryName;
    // Another library starts from the default order and filter: a filter its type cannot offer would silently empty the grid
    if (initial)
        applyViewState(*initial);
    else if (!sameLibrary)
        applyViewState({});
    if (m_filter == Resumable && !resumableAvailable())
        setFilterQuietly(All);
    Q_EMIT libraryChanged();
    restartPaging();
    m_genreOptions.load(libraryId, base->includeItemTypes);
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
    m_genreOptions.clear();
    clearPaging();
    m_libraryId.clear();
    m_collectionType.clear();
    m_libraryName.clear();
    if (hadLibrary)
        Q_EMIT libraryChanged();
}

void LibraryItemsModel::applyViewState(const ViewState &state)
{
    if (m_sortKey != state.sort) {
        m_sortKey = state.sort;
        Q_EMIT sortKeyChanged();
    }
    if (m_descending != state.descending) {
        m_descending = state.descending;
        Q_EMIT descendingChanged();
    }
    setFilterQuietly(state.filter);
    // Genres belong to a library: another one starts with its own or none
    assignGenreIds(state.genreIds);
}

void LibraryItemsModel::setFilterQuietly(Filter filter)
{
    if (filter == m_filter)
        return;
    m_filter = filter;
    Q_EMIT filterChanged();
    Q_EMIT filteredChanged();
}

void LibraryItemsModel::setSortKey(SortKey key)
{
    if (key == m_sortKey)
        return;
    m_sortKey = key;
    Q_EMIT sortKeyChanged();
    if (isOpen())
        restartPaging();
    Q_EMIT viewStateEdited();
}

void LibraryItemsModel::setDescending(bool descending)
{
    if (descending == m_descending)
        return;
    m_descending = descending;
    Q_EMIT descendingChanged();
    if (isOpen())
        restartPaging();
    Q_EMIT viewStateEdited();
}

void LibraryItemsModel::setFilter(Filter filter)
{
    if (filter == m_filter)
        return;
    m_filter = filter;
    Q_EMIT filterChanged();
    Q_EMIT filteredChanged();
    if (isOpen())
        restartPaging();
    Q_EMIT viewStateEdited();
}

void LibraryItemsModel::setGenreIds(const QStringList &ids)
{
    if (!assignGenreIds(ids))
        return;
    if (isOpen())
        restartPaging();
    Q_EMIT viewStateEdited();
}

bool LibraryItemsModel::assignGenreIds(const QStringList &ids)
{
    // Valid ids only, each once, at most as many as a query may carry
    QStringList clean;
    for (const QString &id : ids) {
        if (isValidJellyfinId(id) && !clean.contains(id, Qt::CaseInsensitive)
            && clean.size() < MaxGenreIds)
            clean.append(id);
    }
    if (clean == m_genreIds)
        return false;
    const bool wasFiltered = filtered();
    m_genreIds = clean;
    Q_EMIT genreIdsChanged();
    if (filtered() != wasFiltered)
        Q_EMIT filteredChanged();
    return true;
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
    const bool wasFiltered = filtered();
    m_appliedTerm = applied;
    if (filtered() != wasFiltered)
        Q_EMIT filteredChanged();
    if (isOpen())
        restartPaging();
}

void LibraryItemsModel::dropSearchTerm()
{
    m_searchDebounce.stop();
    const bool wasFiltered = filtered();
    const bool hadTerm = !m_searchTerm.isEmpty();
    m_searchTerm.clear();
    m_appliedTerm.clear();
    if (hadTerm)
        Q_EMIT searchTermChanged();
    if (filtered() != wasFiltered)
        Q_EMIT filteredChanged();
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
                                 {LoadedRole, "loaded"}};
    names.insert(cardRoleNames());
    return names;
}

QVariant LibraryItemsModel::rowData(const ItemSummary &item, bool loaded, int role) const
{
    switch (role) {
    case LoadedRole:
        return loaded;
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
        return item.valid && m_client && !item.primaryTag.isEmpty()
                   ? m_client->imageUrl(item.id, QStringLiteral("Primary"), ImageWidth,
                                        item.primaryTag)
                   : QUrl();
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

bool LibraryItemsModel::sendPage(int start, int limit)
{
    ItemQuery query = m_base;
    query.sortBy = sortByFor(m_sortKey);
    query.sortOrder = m_descending ? QStringLiteral("Descending") : QStringLiteral("Ascending");
    query.filters = filtersFor(m_filter);
    query.genreIds = m_genreIds;
    query.searchTerm = m_appliedTerm;
    query.startIndex = start;
    query.limit = limit;
    return m_client && m_client->fetchItems(query, m_channel);
}

void LibraryItemsModel::onItemsReady(const QJsonArray &items, int total, int channel)
{
    if (channel != m_channel || !pageInFlight() || !isOpen())
        return;
    deliverPage(items, total);
}

void LibraryItemsModel::onListingFailed(const QString &family, int, const QString &, int channel)
{
    if (family != QLatin1String("Items") || channel != m_channel || !pageInFlight() || !isOpen())
        return;
    failPageInFlight();
}
} // namespace Kinefin
