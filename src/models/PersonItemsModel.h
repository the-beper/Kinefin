// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/JellyfinClient.h"
#include "models/CardRoles.h"
#include "models/ItemRowModel.h"
#include "models/PagedItemsModel.h"

namespace Kinefin {
// The items one person appears in, as a paged list of landscape cards. `open()` loads the first page for a person id; the page that shows it calls `close()` when it goes. It uses its own listing channel, so no other Items consumer sees its replies
class PersonItemsModel final : public PagedItemsModel {
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
    static constexpr int Channel = 6146;
    static constexpr int ImageWidth = ItemRowModel::LandscapeWidth;

    explicit PersonItemsModel(JellyfinClient *client, QObject *parent = nullptr);

    // Resets the model and loads the first page for the person; false (and the model stays empty) when the request is refused
    Q_INVOKABLE bool open(const QString &personId);
    // Empties the model and releases the listing channel
    Q_INVOKABLE void close();

    QHash<int, QByteArray> roleNames() const override;

  protected:
    bool sendPage(int start, int limit) override;
    QVariant rowData(const ItemSummary &item, bool loaded, int role) const override;

  private:
    void onItemsReady(const QJsonArray &items, int total, int channel);
    void onListingFailed(const QString &family, int status, const QString &reason, int channel);

    JellyfinClient *m_client;
    QString m_personId;
    bool m_open = false;
};
} // namespace Kinefin
