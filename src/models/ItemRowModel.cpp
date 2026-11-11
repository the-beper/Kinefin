// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ItemRowModel.h"

#include <QJsonValue>

namespace Kinefin {

ItemRowModel::ItemRowModel(JellyfinClient *client, bool landscape, QObject *parent,
                           bool seasonCounts)
    : QAbstractListModel(parent), m_client(client), m_landscape(landscape),
      m_seasonCounts(seasonCounts)
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
                                 {FavoriteRole, "favorite"}};
    names.insert(cardRoleNames());
    return names;
}

QUrl ItemRowModel::imageUrlFor(const ItemSummary &item) const
{
    return cardImageUrl(m_client, item, m_landscape, m_landscape ? LandscapeWidth : PosterWidth);
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
    case SeasonCountRole:
    case EpisodeCountRole:
    case SeriesProgressRole:
        if (!m_seasonCounts)
            return role == SeriesProgressRole ? QVariant(qreal(-1)) : QVariant(-1);
        return cardRoleData(item, role);
    default:
        return cardRoleData(item, role);
    }
}
} // namespace Kinefin
