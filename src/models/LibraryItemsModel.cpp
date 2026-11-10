// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "LibraryItemsModel.h"

#include "api/MediaSourceSelection.h"

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

LibraryItemsModel::LibraryItemsModel(JellyfinClient *client, QObject *parent)
    : PagedItemsModel(PageSize, parent), m_client(client)
{
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
    if (collectionType == QLatin1String("movies"))
        query.includeItemTypes = QStringLiteral("Movie");
    else if (collectionType == QLatin1String("tvshows"))
        query.includeItemTypes = QStringLiteral("Series");
    else if (collectionType == QLatin1String("boxsets"))
        query.includeItemTypes = QStringLiteral("BoxSet");
    else
        return std::nullopt;
    return query;
}

bool LibraryItemsModel::open(const QString &libraryId, const QString &collectionType,
                             const QString &libraryName)
{
    const auto base = browseQuery(collectionType);
    const bool sameLibrary = libraryId == m_libraryId;
    clear();
    if (!base || !isValidJellyfinId(libraryId))
        return false;
    m_base = *base;
    m_base.parentId = libraryId;
    m_libraryId = libraryId;
    m_collectionType = collectionType;
    m_libraryName = libraryName;
    // Another library starts from the default order and filter: a filter its type cannot offer would silently empty the grid
    if (!sameLibrary)
        resetOrderAndFilter();
    else if (m_filter == Resumable && !resumableAvailable())
        setFilterQuietly(All);
    Q_EMIT libraryChanged();
    restartPaging();
    return true;
}

void LibraryItemsModel::clear()
{
    const bool hadLibrary = !m_libraryId.isEmpty();
    clearPaging();
    m_libraryId.clear();
    m_collectionType.clear();
    m_libraryName.clear();
    if (hadLibrary)
        Q_EMIT libraryChanged();
}

void LibraryItemsModel::resetOrderAndFilter()
{
    if (m_sortKey != Name) {
        m_sortKey = Name;
        Q_EMIT sortKeyChanged();
    }
    if (m_descending) {
        m_descending = false;
        Q_EMIT descendingChanged();
    }
    setFilterQuietly(All);
}

void LibraryItemsModel::setFilterQuietly(Filter filter)
{
    if (filter == m_filter)
        return;
    m_filter = filter;
    Q_EMIT filterChanged();
}

void LibraryItemsModel::setSortKey(SortKey key)
{
    if (key == m_sortKey)
        return;
    m_sortKey = key;
    Q_EMIT sortKeyChanged();
    if (!m_libraryId.isEmpty())
        restartPaging();
}

void LibraryItemsModel::setDescending(bool descending)
{
    if (descending == m_descending)
        return;
    m_descending = descending;
    Q_EMIT descendingChanged();
    if (!m_libraryId.isEmpty())
        restartPaging();
}

void LibraryItemsModel::setFilter(Filter filter)
{
    if (filter == m_filter)
        return;
    m_filter = filter;
    Q_EMIT filterChanged();
    if (!m_libraryId.isEmpty())
        restartPaging();
}

QHash<int, QByteArray> LibraryItemsModel::roleNames() const
{
    return {{ItemIdRole, "itemId"},
            {NameRole, "name"},
            {YearRole, "year"},
            {TypeRole, "type"},
            {ImageUrlRole, "imageUrl"},
            {PlayedRole, "played"},
            {UnplayedCountRole, "unplayedCount"},
            {PlayedPercentageRole, "playedPercentage"},
            {FavoriteRole, "favorite"},
            {LoadedRole, "loaded"}};
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
        return {};
    }
}

bool LibraryItemsModel::sendPage(int start, int limit)
{
    ItemQuery query = m_base;
    query.sortBy = sortByFor(m_sortKey);
    query.sortOrder = m_descending ? QStringLiteral("Descending") : QStringLiteral("Ascending");
    query.filters = filtersFor(m_filter);
    query.startIndex = start;
    query.limit = limit;
    return m_client && m_client->fetchItems(query, Channel);
}

void LibraryItemsModel::onItemsReady(const QJsonArray &items, int total, int channel)
{
    if (channel != Channel || !pageInFlight() || m_libraryId.isEmpty())
        return;
    deliverPage(items, total);
}

void LibraryItemsModel::onListingFailed(const QString &family, int, const QString &, int channel)
{
    if (family != QLatin1String("Items") || channel != Channel || !pageInFlight()
        || m_libraryId.isEmpty())
        return;
    failPageInFlight();
}
} // namespace Kinefin
