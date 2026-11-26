// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/JellyfinClient.h"
#include "models/ItemSummary.h"

#include <QAbstractListModel>

#include <functional>

namespace Kinefin {
// The episodes one person appears in within one series, as the rows of an episode list ordered by season and episode number (a season without a number last). It has the roles of `SeasonEpisodesModel`, so the same row component reads both; the seasons are the `seasonNumber` role, which a view turns into section headers. `PersonItemsModel` owns it and fills it
class PersonEpisodesModel final : public QAbstractListModel {
    Q_OBJECT

  public:
    explicit PersonEpisodesModel(JellyfinClient *client, QObject *parent = nullptr);

    // Replaces the rows with these episodes, sorted
    void setEpisodes(QList<ItemSummary> episodes);
    void clear();
    // Applies `change` to the rows of an item, which refreshes every role of them
    void update(const QString &itemId, const std::function<void(ItemSummary &)> &change);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

  private:
    JellyfinClient *m_client;
    QList<ItemSummary> m_episodes;
};
} // namespace Kinefin
