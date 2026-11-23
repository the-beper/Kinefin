// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "PersonItemsModel.h"

#include <QJsonArray>

namespace Kinefin {

PersonItemsModel::PersonItemsModel(JellyfinClient *client, QObject *parent)
    : PagedItemsModel(PageSize, parent), m_client(client)
{
    if (m_client) {
        connect(m_client, &JellyfinClient::itemsReady, this, &PersonItemsModel::onItemsReady);
        connect(m_client, &JellyfinClient::listingFailed, this, &PersonItemsModel::onListingFailed);
        // A manual watched-state edit changes the badges of the resident rows
        connect(m_client, &JellyfinClient::itemPlayedUpdated, this,
                [this](const QString &id, bool played) {
                    if (m_open)
                        applyPlayedState(id, played);
                });
        connect(m_client, &JellyfinClient::itemUnplayedCountUpdated, this,
                [this](const QString &id, int count) {
                    if (m_open)
                        applyUnplayedCountState(id, count);
                });
        connect(m_client, &JellyfinClient::itemFavoriteUpdated, this,
                [this](const QString &id, bool favorite) {
                    applyFavorite(id, favorite, FavoriteRole);
                });
        // A 401, a disconnect, a server switch and a sign-out all end with this signal
        connect(m_client, &JellyfinClient::credentialsCleared, this, [this] { close(); });
    }
}

bool PersonItemsModel::open(const QString &personId)
{
    if (!m_client || personId.isEmpty())
        return false;
    m_personId = personId;
    m_open = true;
    restartPaging();
    return status() != Failed;
}

void PersonItemsModel::close()
{
    m_open = false;
    m_personId.clear();
    clearPaging();
    if (m_client)
        m_client->releaseListingChannel(Channel);
}

QHash<int, QByteArray> PersonItemsModel::roleNames() const
{
    QHash<int, QByteArray> names{{ItemIdRole, "itemId"},
                                 {NameRole, "name"},
                                 {EpisodeTitleRole, "episodeTitle"},
                                 {SeasonNumberRole, "seasonNumber"},
                                 {EpisodeNumberRole, "episodeNumber"},
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

QVariant PersonItemsModel::rowData(const ItemSummary &item, bool loaded, int role) const
{
    const bool episode = item.type == QLatin1String("Episode");
    switch (role) {
    case LoadedRole:
        return loaded;
    case Qt::DisplayRole:
    case NameRole:
        return episode && !item.seriesName.isEmpty() ? item.seriesName : item.name;
    case EpisodeTitleRole:
        return episode ? item.name : QString();
    case SeasonNumberRole:
        return item.seasonNumber;
    case EpisodeNumberRole:
        return item.episodeNumber;
    case ItemIdRole:
        return item.id;
    case YearRole:
        return item.year;
    case TypeRole:
        return item.type;
    case ImageUrlRole:
        return item.valid && m_client ? cardImageUrl(m_client, item, true, ImageWidth) : QUrl();
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

bool PersonItemsModel::sendPage(int start, int limit)
{
    if (!m_client || m_personId.isEmpty())
        return false;
    ItemQuery query;
    query.personIds = {m_personId};
    query.recursive = true;
    query.includeItemTypes = QStringLiteral("Movie,Series,Episode");
    query.sortBy = QStringLiteral("SortName");
    query.cardImages = true;
    query.startIndex = start;
    query.limit = limit;
    return m_client->fetchItems(query, Channel);
}

void PersonItemsModel::onItemsReady(const QJsonArray &items, int total, int channel)
{
    if (channel != Channel || !pageInFlight() || !m_open)
        return;
    deliverPage(items, total);
}

void PersonItemsModel::onListingFailed(const QString &family, int, const QString &, int channel)
{
    if (family != QLatin1String("Items") || channel != Channel || !pageInFlight() || !m_open)
        return;
    failPageInFlight();
}
} // namespace Kinefin
