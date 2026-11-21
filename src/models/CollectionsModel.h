// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/JellyfinClient.h"
#include "models/CardRoles.h"
#include "models/ItemRowModel.h"
#include "models/PagedItemsModel.h"

namespace Kinefin {
// The user's collections (BoxSets) as a paged list for the Add to Collection picker; the page that shows it calls open() and close(). Its own listing channel
class CollectionsModel final : public PagedItemsModel {
    Q_OBJECT

  public:
    enum Role { ItemIdRole = Qt::UserRole + 1, NameRole, ImageUrlRole, LoadedRole };

    static constexpr int PageSize = 100;
    static constexpr int Channel = 6147;
    static constexpr int ImageWidth = ItemRowModel::PosterWidth;

    explicit CollectionsModel(JellyfinClient *client, QObject *parent = nullptr);

    // Resets the model and loads the first page; false when not usable
    Q_INVOKABLE bool open();
    Q_INVOKABLE void close();

    QHash<int, QByteArray> roleNames() const override;

  protected:
    bool sendPage(int start, int limit) override;
    QVariant rowData(const ItemSummary &item, bool loaded, int role) const override;

  private:
    void onItemsReady(const QJsonArray &items, int total, int channel);
    void onListingFailed(const QString &family, int status, const QString &reason, int channel);

    JellyfinClient *m_client;
    bool m_open = false;
};
} // namespace Kinefin
