// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/JellyfinClient.h"
#include "models/ItemRowModel.h"

#include <QAbstractListModel>
#include <QJsonArray>

#include <vector>

namespace Kinefin {
// The movie recommendation rows of `GET /Movies/Recommendations`: a list of categories, each with the items model of its row. Everything comes from the server and is untrusted: at most `MaxCategories` rows, a row without a valid item is left out, the baseline name is capped and shown as plain text (never as markup), the category id is kept only when it is a valid id and the recommendation type only when it is one of the server's known kinds (the view builds the title from `kind` and `baselineName`, so the wording stays translatable). Not a PagedItemsModel: the endpoint answers every row at once
class RecommendationRowsModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

  public:
    // The server's RecommendationType values; Other is anything else
    enum Kind {
        Other,
        SimilarToRecentlyPlayed,
        SimilarToLikedItem,
        HasDirectorFromRecentlyPlayed,
        HasActorFromRecentlyPlayed,
        HasLikedDirector,
        HasLikedActor
    };
    Q_ENUM(Kind)
    enum Role {
        KindRole = Qt::UserRole + 1,
        BaselineNameRole,
        CategoryIdRole,
        ItemsRole,
        CountRole
    };

    static constexpr int MaxCategories = 20;

    explicit RecommendationRowsModel(JellyfinClient *client, QObject *parent = nullptr);

    // Replaces every row with the categories of one reply
    void setCategories(const QJsonArray &categories);
    void clear();

    int count() const { return int(m_rows.size()); }
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

  Q_SIGNALS:
    void countChanged();

  private:
    struct Row {
        Kind kind = Other;
        QString baselineName;
        QString categoryId;
        ItemRowModel *items = nullptr;
    };

    JellyfinClient *m_client;
    std::vector<Row> m_rows;
};
} // namespace Kinefin
