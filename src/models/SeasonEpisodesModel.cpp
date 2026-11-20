// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "SeasonEpisodesModel.h"

#include <QJsonArray>

namespace Kinefin {
namespace {
QUrl episodeImage(const JellyfinClient *client, const ItemSummary &item)
{
    if (!client || !item.valid)
        return {};
    // The episode's own image, else the backdrop of its parent
    if (!item.primaryTag.isEmpty())
        return client->imageUrl(item.id, QStringLiteral("Primary"), SeasonEpisodesModel::ImageWidth,
                                item.primaryTag);
    if (!item.parentBackdropItemId.isEmpty() && !item.parentBackdropTag.isEmpty())
        return client->imageUrl(item.parentBackdropItemId, QStringLiteral("Backdrop"),
                                SeasonEpisodesModel::ImageWidth, item.parentBackdropTag);
    return {};
}

QVariant episodeValue(const JellyfinClient *client, const ItemSummary &item, bool loaded, int role)
{
    switch (role) {
    case SeasonEpisodesModel::LoadedRole:
        return loaded;
    case Qt::DisplayRole:
    case SeasonEpisodesModel::NameRole:
        return item.name;
    case SeasonEpisodesModel::ItemIdRole:
        return item.id;
    case SeasonEpisodesModel::SeasonNumberRole:
        return item.seasonNumber;
    case SeasonEpisodesModel::EpisodeNumberRole:
        return item.episodeNumber;
    case SeasonEpisodesModel::EpisodeNumberEndRole:
        return item.episodeNumberEnd;
    case SeasonEpisodesModel::RuntimeMinutesRole:
        return item.runtimeMinutes;
    case SeasonEpisodesModel::OverviewRole:
        return item.overview;
    case SeasonEpisodesModel::ImageUrlRole:
        return episodeImage(client, item);
    case SeasonEpisodesModel::PlayedRole:
        return item.played;
    case SeasonEpisodesModel::PlayedPercentageRole:
        return item.playedPercentage;
    case SeasonEpisodesModel::FavoriteRole:
        return item.favorite;
    default:
        return {};
    }
}
} // namespace

SeasonEpisodesModel::SeasonEpisodesModel(JellyfinClient *client, int channel, QObject *parent)
    : PagedItemsModel(PageSize, parent), m_client(client), m_channel(channel)
{
    if (m_client) {
        connect(m_client, &JellyfinClient::episodesReady, this,
                &SeasonEpisodesModel::onEpisodesReady);
        connect(m_client, &JellyfinClient::listingFailed, this,
                &SeasonEpisodesModel::onListingFailed);
        connect(m_client, &JellyfinClient::itemFavoriteUpdated, this,
                [this](const QString &id, bool favorite) {
                    applyFavorite(id, favorite, FavoriteRole);
                });
        connect(m_client, &JellyfinClient::itemPlayedUpdated, this,
                [this](const QString &id, bool played) { applyPlayedState(id, played); });
        connect(m_client, &JellyfinClient::itemProgressUpdated, this,
                [this](const QString &id, qint64 position, double percentage) {
                    applyProgressState(id, position, percentage);
                });
        // A 401, a disconnect, a server switch and a sign-out all end with this signal
        connect(m_client, &JellyfinClient::credentialsCleared, this, [this] { clear(); });
    }
}

void SeasonEpisodesModel::open(const QString &seriesId, const QString &seasonId)
{
    clear();
    m_seriesId = seriesId;
    m_seasonId = seasonId;
    // An id the server could not have sent is refused by the client without a request: the model ends up Failed
    restartPaging();
}

void SeasonEpisodesModel::clear()
{
    clearPaging();
    m_seriesId.clear();
    m_seasonId.clear();
}

QHash<int, QByteArray> SeasonEpisodesModel::roleNameTable()
{
    return {{ItemIdRole, "itemId"},
            {NameRole, "name"},
            {SeasonNumberRole, "seasonNumber"},
            {EpisodeNumberRole, "episodeNumber"},
            {EpisodeNumberEndRole, "episodeNumberEnd"},
            {RuntimeMinutesRole, "runtimeMinutes"},
            {OverviewRole, "overview"},
            {ImageUrlRole, "imageUrl"},
            {PlayedRole, "played"},
            {PlayedPercentageRole, "playedPercentage"},
            {FavoriteRole, "favorite"},
            {LoadedRole, "loaded"}};
}

QHash<int, QByteArray> SeasonEpisodesModel::roleNames() const
{
    return roleNameTable();
}

QVariantMap SeasonEpisodesModel::rowMap(const JellyfinClient *client, const ItemSummary &item)
{
    // The same names and values a row of the model has, so the Next Up entry reads like one
    QVariantMap map;
    const QHash<int, QByteArray> names = roleNameTable();
    for (auto it = names.constBegin(); it != names.constEnd(); ++it)
        map.insert(QString::fromLatin1(it.value()), episodeValue(client, item, true, it.key()));
    return map;
}

QVariant SeasonEpisodesModel::rowData(const ItemSummary &item, bool loaded, int role) const
{
    return episodeValue(m_client, item, loaded, role);
}

bool SeasonEpisodesModel::sendPage(int start, int limit)
{
    return m_client && m_client->fetchEpisodes(m_seriesId, m_seasonId, start, limit, m_channel);
}

void SeasonEpisodesModel::onEpisodesReady(const QJsonArray &items, int total, int channel)
{
    if (channel != m_channel || !pageInFlight() || m_seriesId.isEmpty())
        return;
    deliverPage(items, total);
}

void SeasonEpisodesModel::onListingFailed(const QString &family, int, const QString &, int channel)
{
    if (family != QLatin1String("Episodes") || channel != m_channel || !pageInFlight()
        || m_seriesId.isEmpty())
        return;
    failPageInFlight();
}
} // namespace Kinefin
