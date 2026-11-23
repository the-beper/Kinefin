// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/JellyfinClient.h"
#include "models/PagedItemsModel.h"

#include <QVariantMap>

namespace Kinefin {
// The episodes of one season as a paged list (see PagedItemsModel), in the server's order: specials and double episodes stay where the server puts them. It listens on the listing channel it is given
class SeasonEpisodesModel final : public PagedItemsModel {
    Q_OBJECT

  public:
    enum Role {
        ItemIdRole = Qt::UserRole + 1,
        NameRole,
        SeasonNumberRole,
        EpisodeNumberRole,
        EpisodeNumberEndRole,
        RuntimeMinutesRole,
        OverviewRole,
        ImageUrlRole,
        PlayedRole,
        PlayedPercentageRole,
        FavoriteRole,
        PositionTicksRole,
        LoadedRole
    };

    static constexpr int PageSize = 50;
    // The width requested for an episode image
    static constexpr int ImageWidth = 480;

    SeasonEpisodesModel(JellyfinClient *client, int channel, QObject *parent = nullptr);

    // Resets the model and loads the first page. A series id the server could not have sent leaves the model Failed
    void open(const QString &seriesId, const QString &seasonId);
    void clear();

    QHash<int, QByteArray> roleNames() const override;
    // The role names, one table for the model and for rowMap()
    static QHash<int, QByteArray> roleNameTable();

    // The values of one episode as a map with the role names: the one owner of how an episode row reads, shared with the Next Up entry of a series page
    static QVariantMap rowMap(const JellyfinClient *client, const ItemSummary &item);

  protected:
    bool sendPage(int start, int limit) override;
    QVariant rowData(const ItemSummary &item, bool loaded, int role) const override;

  private:
    void onEpisodesReady(const QJsonArray &items, int total, int channel);
    void onListingFailed(const QString &family, int status, const QString &reason, int channel);

    JellyfinClient *m_client;
    int m_channel;
    QString m_seriesId;
    QString m_seasonId;
};
} // namespace Kinefin
