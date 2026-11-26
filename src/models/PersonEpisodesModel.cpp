// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "PersonEpisodesModel.h"

#include "models/SeasonEpisodesModel.h"

#include <algorithm>
#include <limits>

namespace Kinefin {

PersonEpisodesModel::PersonEpisodesModel(JellyfinClient *client, QObject *parent)
    : QAbstractListModel(parent), m_client(client)
{
}

void PersonEpisodesModel::setEpisodes(QList<ItemSummary> episodes)
{
    // A number of -1 (the server sent none) sorts after every real one
    const auto number = [](int value) {
        return value < 0 ? std::numeric_limits<int>::max() : value;
    };
    std::stable_sort(episodes.begin(), episodes.end(),
                     [&](const ItemSummary &a, const ItemSummary &b) {
                         if (a.seasonNumber != b.seasonNumber)
                             return number(a.seasonNumber) < number(b.seasonNumber);
                         return number(a.episodeNumber) < number(b.episodeNumber);
                     });
    beginResetModel();
    m_episodes = std::move(episodes);
    endResetModel();
}

void PersonEpisodesModel::clear()
{
    setEpisodes({});
}

void PersonEpisodesModel::update(const QString &itemId,
                                 const std::function<void(ItemSummary &)> &change)
{
    for (qsizetype row = 0; row < m_episodes.size(); ++row) {
        if (!sameItemId(m_episodes.at(row).id, itemId))
            continue;
        change(m_episodes[row]);
        Q_EMIT dataChanged(index(int(row)), index(int(row)));
    }
}

int PersonEpisodesModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_episodes.size());
}

QVariant PersonEpisodesModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_episodes.size())
        return {};
    const int wanted = role == Qt::DisplayRole ? int(SeasonEpisodesModel::NameRole) : role;
    return SeasonEpisodesModel::roleValue(m_client, m_episodes.at(index.row()), wanted);
}

QHash<int, QByteArray> PersonEpisodesModel::roleNames() const
{
    return SeasonEpisodesModel::roleNameTable();
}
} // namespace Kinefin
