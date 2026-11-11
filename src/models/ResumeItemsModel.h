// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/JellyfinClient.h"
#include "models/CardRoles.h"
#include "models/ItemRowModel.h"
#include "models/PagedItemsModel.h"

namespace Kinefin {
// Everything in Continue Watching as a paged list (see PagedItemsModel, with the card roles of CardRoles.h): the "See All" page of the Home row. It uses its own listing channel, so the Home row never sees its replies. `open()` loads the first page, `close()` empties the model and frees the channel, and the model also empties itself whenever the client loses its credentials
class ResumeItemsModel final : public PagedItemsModel {
    Q_OBJECT

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
        FavoriteRole,
        LoadedRole
    };

    static constexpr int PageSize = 100;
    static constexpr int Channel = 8;
    // The width requested for a landscape card, as the Home row does
    static constexpr int ImageWidth = ItemRowModel::LandscapeWidth;

    ResumeItemsModel(JellyfinClient *client, QObject *parent = nullptr);

    // Resets the model and loads the first page; false (and the model stays empty) when the request is refused
    bool open();
    // Empties the model and releases the listing channel
    Q_INVOKABLE void close();

    QHash<int, QByteArray> roleNames() const override;

  protected:
    bool sendPage(int start, int limit) override;
    QVariant rowData(const ItemSummary &item, bool loaded, int role) const override;

  private:
    void onResumeReady(const QJsonArray &items, int total, int channel);
    void onListingFailed(const QString &family, int status, const QString &reason, int channel);

    JellyfinClient *m_client;
    bool m_open = false;
};
} // namespace Kinefin
