// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/JellyfinClient.h"
#include "models/CardRoles.h"
#include "models/ItemSummary.h"

#include <QAbstractListModel>
#include <QJsonArray>

#include <vector>

namespace Kinefin {
// The items of one home row: a short flat list the server answered in one reply (no paging). Entries without a valid id are dropped, and a row never holds more than `MaxItems`. A landscape row shows wide images: the thumb or backdrop of a movie or series and the own image of an episode; a poster row shows the primary image. Besides its own roles the model has the card roles (see CardRoles.h); with `seasonCounts` false the series roles stay -1, for the rows whose `ChildCount` does not count seasons
class ItemRowModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(bool landscape READ landscape CONSTANT)

  public:
    enum Role {
        ItemIdRole = Qt::UserRole + 1,
        NameRole,
        EpisodeTitleRole,
        SeasonNumberRole,
        EpisodeNumberRole,
        YearRole,
        TypeRole,
        ImageUrlRole,
        PlayedRole,
        UnplayedCountRole,
        PlayedPercentageRole,
        FavoriteRole
    };

    static constexpr int MaxItems = 100;
    // The widths requested for poster and landscape images
    static constexpr int PosterWidth = 400;
    static constexpr int LandscapeWidth = 480;

    // `seasonCounts` comes after `parent` so that a pointer is never taken for it
    ItemRowModel(JellyfinClient *client, bool landscape, QObject *parent = nullptr,
                 bool seasonCounts = true);

    bool landscape() const { return m_landscape; }
    int count() const { return int(m_items.size()); }
    void setItems(const QJsonArray &items);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

  Q_SIGNALS:
    void countChanged();

  private:
    QUrl imageUrlFor(const ItemSummary &item) const;

    JellyfinClient *m_client;
    bool m_landscape;
    bool m_seasonCounts;
    std::vector<ItemSummary> m_items;
};
} // namespace Kinefin
