// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ResumeItemsModel.h"

#include <QJsonArray>

namespace Kinefin {

ResumeItemsModel::ResumeItemsModel(JellyfinClient *client, QObject *parent)
    : PagedItemsModel(PageSize, parent), m_client(client)
{
    if (m_client) {
        connect(m_client, &JellyfinClient::resumeItemsReady, this,
                &ResumeItemsModel::onResumeReady);
        connect(m_client, &JellyfinClient::listingFailed, this, &ResumeItemsModel::onListingFailed);
        // A manual watched-state edit can remove an entry from Continue Watching.
        connect(m_client, &JellyfinClient::itemPlayedUpdated, this, [this] {
            if (m_open)
                restartPaging();
        });
        // A 401, a disconnect, a server switch and a sign-out all end with this signal
        connect(m_client, &JellyfinClient::credentialsCleared, this, [this] { close(); });
    }
}

bool ResumeItemsModel::open()
{
    m_open = true;
    restartPaging();
    return status() != Failed;
}

void ResumeItemsModel::close()
{
    m_open = false;
    clearPaging();
    if (m_client)
        m_client->releaseListingChannel(Channel);
}

QHash<int, QByteArray> ResumeItemsModel::roleNames() const
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

QVariant ResumeItemsModel::rowData(const ItemSummary &item, bool loaded, int role) const
{
    const bool episode = item.type == QLatin1String("Episode");
    switch (role) {
    case LoadedRole:
        return loaded;
    case Qt::DisplayRole:
    case NameRole:
        // An episode is listed under its series
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

bool ResumeItemsModel::sendPage(int start, int limit)
{
    return m_client && m_client->fetchResumeItems(limit, Channel, start);
}

void ResumeItemsModel::onResumeReady(const QJsonArray &items, int total, int channel)
{
    if (channel != Channel || !pageInFlight() || !m_open)
        return;
    deliverPage(items, total);
}

void ResumeItemsModel::onListingFailed(const QString &family, int, const QString &, int channel)
{
    if (family != QLatin1String("Resume") || channel != Channel || !pageInFlight() || !m_open)
        return;
    failPageInFlight();
}
} // namespace Kinefin
