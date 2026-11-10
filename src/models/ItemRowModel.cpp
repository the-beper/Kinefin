// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ItemRowModel.h"

#include <QJsonValue>

namespace Kinefin {

ItemRowModel::ItemRowModel(JellyfinClient *client, bool landscape, QObject *parent)
    : QAbstractListModel(parent), m_client(client), m_landscape(landscape)
{
}

void ItemRowModel::setItems(const QJsonArray &items)
{
    std::vector<ItemSummary> parsed;
    for (const QJsonValue &value : items) {
        if (int(parsed.size()) == MaxItems)
            break;
        ItemSummary summary = parseItemSummary(value.toObject());
        if (summary.valid)
            parsed.push_back(std::move(summary));
    }
    const bool countChangedNow = parsed.size() != m_items.size();
    beginResetModel();
    m_items = std::move(parsed);
    endResetModel();
    if (countChangedNow)
        Q_EMIT countChanged();
}

int ItemRowModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : count();
}

QHash<int, QByteArray> ItemRowModel::roleNames() const
{
    return {{ItemIdRole, "itemId"},
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
            {FavoriteRole, "favorite"}};
}

QUrl ItemRowModel::imageUrlFor(const ItemSummary &item) const
{
    if (!m_client)
        return {};
    const bool episode = item.type == QLatin1String("Episode");
    const int width = m_landscape ? LandscapeWidth : PosterWidth;
    const auto url = [&](const QString &id, const QString &type, const QString &tag) {
        return id.isEmpty() || tag.isEmpty() ? QUrl() : m_client->imageUrl(id, type, width, tag);
    };
    if (m_landscape) {
        // jellyfin-web's default for wide rows: an episode shows its series' thumb, then its parent's backdrop, and only then its own image; a movie or series shows its own thumb, then its backdrop
        const QList<QUrl> candidates =
            episode ? QList<QUrl>{url(item.seriesId, QStringLiteral("Thumb"), item.seriesThumbTag),
                                  url(item.parentBackdropItemId, QStringLiteral("Backdrop"),
                                      item.parentBackdropTag)}
                    : QList<QUrl>{url(item.id, QStringLiteral("Thumb"), item.thumbTag),
                                  url(item.id, QStringLiteral("Backdrop"), item.backdropTag)};
        for (const QUrl &candidate : candidates) {
            if (!candidate.isEmpty())
                return candidate;
        }
    }
    return url(item.id, QStringLiteral("Primary"), item.primaryTag);
}

QVariant ItemRowModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.parent().isValid() || index.row() < 0 || index.row() >= count())
        return {};
    const ItemSummary &item = m_items[size_t(index.row())];
    const bool episode = item.type == QLatin1String("Episode");
    switch (role) {
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
        return imageUrlFor(item);
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
} // namespace Kinefin
