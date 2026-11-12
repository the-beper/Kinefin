// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "GenreRailsModel.h"

#include "api/MediaSourceSelection.h"
#include "models/LibraryItemsModel.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>

namespace Kinefin {

GenreRailsModel::GenreRailsModel(JellyfinClient *client, QObject *parent)
    : QAbstractListModel(parent), m_client(client)
{
    if (m_client) {
        connect(m_client, &JellyfinClient::genresReady, this, &GenreRailsModel::onGenresReady);
        connect(m_client, &JellyfinClient::itemsReady, this, &GenreRailsModel::onItemsReady);
        connect(m_client, &JellyfinClient::listingFailed, this, &GenreRailsModel::onListingFailed);
        // A 401, a disconnect, a server switch and a sign-out all end with this signal
        connect(m_client, &JellyfinClient::credentialsCleared, this, [this] { clear(); });
    }
}

GenreRailsModel::~GenreRailsModel()
{
    releaseSlots();
    if (m_client)
        m_client->releaseListingChannel(ListChannel);
}

bool GenreRailsModel::open(const QString &collectionType, const QString &libraryId)
{
    const auto base = LibraryItemsModel::browseQuery(collectionType);
    clear();
    if (!base || (!libraryId.isEmpty() && !isValidJellyfinId(libraryId)))
        return false;
    m_open = true;
    m_libraryId = libraryId;
    m_includeTypes = base->includeItemTypes;
    Q_EMIT libraryChanged();
    requestGenres();
    return true;
}

void GenreRailsModel::clear()
{
    releaseSlots();
    if (m_client)
        m_client->releaseListingChannel(ListChannel);
    if (!m_rails.empty()) {
        beginResetModel();
        const std::vector<Rail> old = std::move(m_rails);
        m_rails.clear();
        endResetModel();
        // The view destroys its delegates after the reset: the inner lists may still be bound to these for a turn
        for (const Rail &rail : old)
            rail.items->deleteLater();
    }
    m_wanted.clear();
    m_more = false;
    m_nextStart = 0;
    const bool hadLibrary = m_open;
    m_open = false;
    m_libraryId.clear();
    m_includeTypes.clear();
    setStatus(Idle);
    if (hadLibrary)
        Q_EMIT libraryChanged();
}

void GenreRailsModel::releaseSlots()
{
    for (int slot = 0; slot < MaxInFlight; ++slot) {
        if (!m_slots[slot].isEmpty() && m_client)
            m_client->releaseListingChannel(FirstSlotChannel + slot);
        m_slots[slot].clear();
    }
}

void GenreRailsModel::requestGenres()
{
    setStatus(Loading);
    if (!m_client
        || !m_client->fetchGenres(m_libraryId, m_nextStart, GenresPerPage, m_includeTypes,
                                  ListChannel))
        setStatus(Failed);
}

void GenreRailsModel::loadMore()
{
    if (canLoadMore())
        requestGenres();
}

void GenreRailsModel::retryList()
{
    if (m_status == Failed && m_open)
        requestGenres();
}

void GenreRailsModel::onGenresReady(const QJsonArray &items, int total, int channel)
{
    if (channel != ListChannel || m_status != Loading)
        return;
    QSet<QString> seen;
    for (const Rail &rail : m_rails)
        seen.insert(rail.genreId.toLower());
    std::vector<Rail> added;
    for (const QJsonValue &entry : items) {
        if (int(m_rails.size() + added.size()) >= MaxRails)
            break;
        const QJsonObject genre = entry.toObject();
        const QString id = genre.value(QStringLiteral("Id")).toString();
        const QString name =
            FilterOptionsModel::cappedName(genre.value(QStringLiteral("Name")).toString());
        if (!isValidJellyfinId(id) || name.isEmpty() || seen.contains(id.toLower()))
            continue;
        seen.insert(id.toLower());
        added.push_back({id, name, RailIdle, -1, new ItemRowModel(m_client, false, this)});
    }
    m_nextStart += int(items.size());
    if (!added.empty()) {
        const int first = int(m_rails.size());
        beginInsertRows(QModelIndex(), first, first + int(added.size()) - 1);
        for (const Rail &rail : added)
            m_rails.push_back(rail);
        endInsertRows();
    }
    // The next page starts after what the server sent, kept or not; a page that added nothing ends the list, or a hostile server could keep it asking
    m_more = !added.empty() && m_nextStart < total && int(m_rails.size()) < MaxRails;
    setStatus(Ready);
    pump();
}

void GenreRailsModel::want(const QString &genreId)
{
    const int row = rowFor(genreId);
    if (row < 0 || m_rails[size_t(row)].status != RailIdle)
        return;
    m_wanted.removeAll(genreId);
    m_wanted.append(genreId);
    pump();
}

void GenreRailsModel::unwant(const QString &genreId)
{
    m_wanted.removeAll(genreId);
}

void GenreRailsModel::retry(int row)
{
    if (row < 0 || row >= int(m_rails.size()) || m_rails[size_t(row)].status != RailFailed)
        return;
    setRailStatus(row, RailIdle);
    m_wanted.removeAll(m_rails[size_t(row)].genreId);
    m_wanted.append(m_rails[size_t(row)].genreId);
    pump();
}

void GenreRailsModel::pump()
{
    while (!m_wanted.isEmpty()) {
        int slot = -1;
        for (int i = 0; i < MaxInFlight && slot < 0; ++i) {
            if (m_slots[i].isEmpty())
                slot = i;
        }
        if (slot < 0)
            return;
        // The newest wanted rail is the one the user is looking at
        const QString genreId = m_wanted.takeLast();
        const int row = rowFor(genreId);
        if (row < 0 || m_rails[size_t(row)].status != RailIdle)
            continue;
        requestRail(row, slot);
    }
}

void GenreRailsModel::requestRail(int row, int slot)
{
    Rail &rail = m_rails[size_t(row)];
    ItemQuery query;
    query.parentId = m_libraryId;
    query.includeItemTypes = m_includeTypes;
    query.recursive = true;
    query.genreIds = {rail.genreId};
    query.limit = ItemsPerRail;
    // jellyfin-web shows its genre rails in no fixed order
    query.sortBy = QStringLiteral("Random");
    query.fields = {QStringLiteral("PrimaryImageAspectRatio")};
    query.cardImages = true;
    rail.slot = slot;
    m_slots[slot] = rail.genreId;
    setRailStatus(row, RailLoading);
    if (!m_client || !m_client->fetchItems(query, FirstSlotChannel + slot)) {
        rail.slot = -1;
        m_slots[slot].clear();
        setRailStatus(row, RailFailed);
    }
}

int GenreRailsModel::rowFor(const QString &genreId) const
{
    for (int row = 0; row < int(m_rails.size()); ++row) {
        if (m_rails[size_t(row)].genreId == genreId)
            return row;
    }
    return -1;
}

int GenreRailsModel::rowForSlot(int slot) const
{
    if (slot < 0 || slot >= MaxInFlight || m_slots[slot].isEmpty())
        return -1;
    return rowFor(m_slots[slot]);
}

void GenreRailsModel::onItemsReady(const QJsonArray &items, int, int channel)
{
    const int slot = channel - FirstSlotChannel;
    const int row = rowForSlot(slot);
    if (row < 0 || m_rails[size_t(row)].status != RailLoading)
        return;
    m_rails[size_t(row)].items->setItems(items);
    m_rails[size_t(row)].slot = -1;
    m_slots[slot].clear();
    setRailStatus(row, RailReady);
    pump();
}

void GenreRailsModel::onListingFailed(const QString &family, int, const QString &, int channel)
{
    const int slot = channel - FirstSlotChannel;
    const int row = rowForSlot(slot);
    if (family == QLatin1String("Genres") && channel == ListChannel && m_status == Loading) {
        setStatus(Failed);
        return;
    }
    if (family != QLatin1String("Items") || row < 0 || m_rails[size_t(row)].status != RailLoading)
        return;
    m_rails[size_t(row)].slot = -1;
    m_slots[slot].clear();
    setRailStatus(row, RailFailed);
    pump();
}

void GenreRailsModel::setStatus(Status status)
{
    if (status == m_status)
        return;
    m_status = status;
    Q_EMIT statusChanged();
}

void GenreRailsModel::setRailStatus(int row, RailStatus status)
{
    Rail &rail = m_rails[size_t(row)];
    if (rail.status == status)
        return;
    rail.status = status;
    const QModelIndex changed = index(row);
    Q_EMIT dataChanged(changed, changed, {RailStatusRole, CountRole, ShownRole});
}

int GenreRailsModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rails.size());
}

QHash<int, QByteArray> GenreRailsModel::roleNames() const
{
    return {{GenreIdRole, "genreId"}, {NameRole, "name"},   {RailStatusRole, "railStatus"},
            {ItemsRole, "items"},     {CountRole, "count"}, {ShownRole, "shown"}};
}

QVariant GenreRailsModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.parent().isValid() || index.row() < 0
        || index.row() >= int(m_rails.size()))
        return {};
    const Rail &rail = m_rails[size_t(index.row())];
    switch (role) {
    case GenreIdRole:
        return rail.genreId;
    case Qt::DisplayRole:
    case NameRole:
        return rail.name;
    case RailStatusRole:
        return rail.status;
    case ItemsRole:
        return QVariant::fromValue<QObject *>(rail.items);
    case CountRole:
        return rail.items->count();
    case ShownRole:
        // Only a rail that loaded empty goes away: one not loaded yet must stay in the layout, or the view would create every delegate at once
        return rail.status != RailReady || rail.items->count() > 0;
    default:
        return {};
    }
}
} // namespace Kinefin
