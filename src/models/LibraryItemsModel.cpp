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
    : QAbstractListModel(parent), m_client(client)
{
    m_pumpTimer.setSingleShot(true);
    m_pumpTimer.setInterval(0);
    connect(&m_pumpTimer, &QTimer::timeout, this, [this] { pump(); });
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
    restart();
    return true;
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

void LibraryItemsModel::resetState()
{
    beginResetModel();
    m_pumpTimer.stop();
    m_pages.clear();
    m_failedPages.clear();
    m_wanted.clear();
    m_residentOrder.clear();
    m_inFlight = -1;
    const bool hadTotal = m_total >= 0;
    m_total = -1;
    endResetModel();
    if (hadTotal)
        Q_EMIT totalCountChanged();
    setPageError(false);
}

void LibraryItemsModel::clear()
{
    const bool hadLibrary = !m_libraryId.isEmpty();
    resetState();
    m_libraryId.clear();
    m_collectionType.clear();
    m_libraryName.clear();
    setStatus(Idle);
    if (hadLibrary)
        Q_EMIT libraryChanged();
}

void LibraryItemsModel::restart()
{
    resetState();
    setStatus(Loading);
    requestPage(0);
}

void LibraryItemsModel::retry()
{
    if (m_libraryId.isEmpty() || m_inFlight >= 0)
        return;
    if (m_total < 0) {
        // The first page never arrived
        setStatus(Loading);
        requestPage(0);
        return;
    }
    const QSet<int> failed = m_failedPages;
    m_failedPages.clear();
    setPageError(false);
    for (const int page : failed)
        noteWanted(page);
}

void LibraryItemsModel::setSortKey(SortKey key)
{
    if (key == m_sortKey)
        return;
    m_sortKey = key;
    Q_EMIT sortKeyChanged();
    if (!m_libraryId.isEmpty())
        restart();
}

void LibraryItemsModel::setDescending(bool descending)
{
    if (descending == m_descending)
        return;
    m_descending = descending;
    Q_EMIT descendingChanged();
    if (!m_libraryId.isEmpty())
        restart();
}

void LibraryItemsModel::setFilter(Filter filter)
{
    if (filter == m_filter)
        return;
    m_filter = filter;
    Q_EMIT filterChanged();
    if (!m_libraryId.isEmpty())
        restart();
}

int LibraryItemsModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : totalCount();
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

QVariant LibraryItemsModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.parent().isValid() || index.row() < 0
        || index.row() >= totalCount())
        return {};
    const int page = index.row() / PageSize;
    const auto resident = m_pages.constFind(page);
    const int offset = index.row() % PageSize;
    const bool loaded = resident != m_pages.constEnd() && offset < int(resident->size());
    if (resident != m_pages.constEnd())
        touch(page);
    else
        noteWanted(page);
    if (role == LoadedRole)
        return loaded && resident->at(offset).valid;
    const ItemSummary empty;
    const ItemSummary &item = loaded ? resident->at(offset) : empty;
    switch (role) {
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

int LibraryItemsModel::rowsInPage(int page) const
{
    return std::clamp(m_total - page * PageSize, 0, int(PageSize));
}

void LibraryItemsModel::noteWanted(int page) const
{
    if (m_status == Idle || m_failedPages.contains(page) || page == m_inFlight
        || m_pages.contains(page))
        return;
    m_wanted.removeAll(page);
    m_wanted.append(page);
    while (m_wanted.size() > MaxWantedPages)
        m_wanted.removeFirst();
    // Not from inside data(): the request goes out on the next event loop turn
    if (!m_pumpTimer.isActive())
        m_pumpTimer.start();
}

void LibraryItemsModel::touch(int page) const
{
    if (!m_residentOrder.isEmpty() && m_residentOrder.last() == page)
        return;
    m_residentOrder.removeAll(page);
    m_residentOrder.append(page);
}

void LibraryItemsModel::pump()
{
    if (m_inFlight >= 0 || m_status == Idle || m_total < 0)
        return;
    while (!m_wanted.isEmpty()) {
        const int page = m_wanted.takeLast();
        if (m_pages.contains(page) || m_failedPages.contains(page) || rowsInPage(page) == 0)
            continue;
        requestPage(page);
        return;
    }
}

void LibraryItemsModel::requestPage(int page)
{
    ItemQuery query = m_base;
    query.sortBy = sortByFor(m_sortKey);
    query.sortOrder = m_descending ? QStringLiteral("Descending") : QStringLiteral("Ascending");
    query.filters = filtersFor(m_filter);
    query.startIndex = page * PageSize;
    query.limit = PageSize;
    m_inFlight = page;
    if (m_client && m_client->fetchItems(query, Channel))
        return;
    // Refused without a request: not authenticated, or a query the client does not accept
    failPage(page);
}

void LibraryItemsModel::onItemsReady(const QJsonArray &items, int total, int channel)
{
    if (channel != Channel || m_inFlight < 0 || m_libraryId.isEmpty())
        return;
    total = std::min(total, MaxRows);
    const int page = m_inFlight;
    m_inFlight = -1;
    if (m_total < 0) {
        // The first page: the total and the placeholders appear together
        if (total > 0) {
            beginInsertRows(QModelIndex(), 0, total - 1);
            m_total = total;
            endInsertRows();
        } else {
            m_total = total;
        }
        Q_EMIT totalCountChanged();
    } else if (total != m_total) {
        // The library changed under the pages: only this reply is trusted
        dropOtherPages(page);
        resizeTo(total);
    }
    m_failedPages.remove(page);
    storePage(page, items);
    setPageError(!m_failedPages.isEmpty());
    setStatus(Ready);
    if (!m_wanted.isEmpty())
        m_pumpTimer.start();
}

void LibraryItemsModel::storePage(int page, const QJsonArray &items)
{
    const int expected = rowsInPage(page);
    if (expected == 0)
        return;
    std::vector<ItemSummary> rows;
    rows.reserve(std::min<qsizetype>(items.size(), expected));
    for (const QJsonValue &value : items) {
        if (int(rows.size()) == expected)
            break;
        rows.push_back(parseItemSummary(value.toObject()));
    }
    const int received = int(rows.size());
    m_pages.insert(page, std::move(rows));
    touch(page);
    if (received < expected) {
        // A short page with an unchanged total: the library ends here, and asking again would loop
        resizeTo(page * PageSize + received);
    }
    const int first = page * PageSize;
    if (received > 0)
        Q_EMIT dataChanged(index(first), index(first + received - 1));
    evictBeyondLimit(page);
}

void LibraryItemsModel::evictBeyondLimit(int keep)
{
    while (m_residentOrder.size() > MaxResidentPages) {
        const int oldest = m_residentOrder.first();
        if (oldest == keep)
            break;
        dropPage(oldest);
    }
}

void LibraryItemsModel::dropPage(int page)
{
    m_residentOrder.removeAll(page);
    const auto resident = m_pages.take(page);
    if (!resident.empty()) {
        const int first = page * PageSize;
        Q_EMIT dataChanged(index(first), index(first + int(resident.size()) - 1));
    }
}

void LibraryItemsModel::dropOtherPages(int keep)
{
    const QList<int> pages = m_pages.keys();
    for (const int page : pages) {
        if (page != keep)
            dropPage(page);
    }
    m_wanted.clear();
    m_failedPages.clear();
}

void LibraryItemsModel::resizeTo(int total)
{
    total = std::max(total, 0);
    const int current = totalCount();
    if (total > current) {
        beginInsertRows(QModelIndex(), current, total - 1);
        m_total = total;
        endInsertRows();
    } else if (total < current) {
        beginRemoveRows(QModelIndex(), total, current - 1);
        m_total = total;
        // Pages entirely past the new end are gone with their rows
        for (const int page : m_pages.keys()) {
            if (page * PageSize >= total) {
                m_pages.remove(page);
                m_residentOrder.removeAll(page);
            } else if (int(m_pages.value(page).size()) > total - page * PageSize) {
                m_pages[page].resize(size_t(total - page * PageSize));
            }
        }
        endRemoveRows();
        // Pages past the new end are gone: they must not keep the error flag or a queued request alive
        m_failedPages.removeIf([this](int page) { return page * PageSize >= m_total; });
        m_wanted.removeIf([this](int page) { return page * PageSize >= m_total; });
        setPageError(!m_failedPages.isEmpty());
    } else {
        return;
    }
    Q_EMIT totalCountChanged();
}

void LibraryItemsModel::onListingFailed(const QString &family, int, const QString &, int channel)
{
    if (family != QLatin1String("Items") || channel != Channel || m_inFlight < 0
        || m_libraryId.isEmpty())
        return;
    failPage(m_inFlight);
}

void LibraryItemsModel::failPage(int page)
{
    m_inFlight = -1;
    if (m_total < 0) {
        setStatus(Failed);
        return;
    }
    m_failedPages.insert(page);
    setPageError(true);
    if (!m_wanted.isEmpty())
        m_pumpTimer.start();
}

void LibraryItemsModel::setStatus(Status status)
{
    if (status == m_status)
        return;
    m_status = status;
    Q_EMIT statusChanged();
}

void LibraryItemsModel::setPageError(bool error)
{
    if (error == m_pageError)
        return;
    m_pageError = error;
    Q_EMIT pageErrorChanged();
}
} // namespace Kinefin
