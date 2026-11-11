// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "RecommendationRowsModel.h"

#include "api/MediaSourceSelection.h"
#include "models/FilterOptionsModel.h"

#include <QJsonObject>
#include <QJsonValue>

namespace Kinefin {
namespace {
RecommendationRowsModel::Kind kindOf(const QString &type)
{
    using Kind = RecommendationRowsModel::Kind;
    if (type == QLatin1String("SimilarToRecentlyPlayed"))
        return Kind::SimilarToRecentlyPlayed;
    if (type == QLatin1String("SimilarToLikedItem"))
        return Kind::SimilarToLikedItem;
    if (type == QLatin1String("HasDirectorFromRecentlyPlayed"))
        return Kind::HasDirectorFromRecentlyPlayed;
    if (type == QLatin1String("HasActorFromRecentlyPlayed"))
        return Kind::HasActorFromRecentlyPlayed;
    if (type == QLatin1String("HasLikedDirector"))
        return Kind::HasLikedDirector;
    if (type == QLatin1String("HasLikedActor"))
        return Kind::HasLikedActor;
    return Kind::Other;
}
} // namespace

RecommendationRowsModel::RecommendationRowsModel(JellyfinClient *client, QObject *parent)
    : QAbstractListModel(parent), m_client(client)
{
}

void RecommendationRowsModel::setCategories(const QJsonArray &categories)
{
    std::vector<Row> rows;
    for (const QJsonValue &entry : categories) {
        if (int(rows.size()) >= MaxCategories)
            break;
        const QJsonObject category = entry.toObject();
        auto *items = new ItemRowModel(m_client, false, this);
        items->setItems(category.value(QStringLiteral("Items")).toArray());
        if (items->count() == 0) {
            delete items;
            continue;
        }
        const QString categoryId = category.value(QStringLiteral("CategoryId")).toString();
        rows.push_back({kindOf(category.value(QStringLiteral("RecommendationType")).toString()),
                        FilterOptionsModel::cappedName(
                            category.value(QStringLiteral("BaselineItemName")).toString()),
                        isValidJellyfinId(categoryId) ? categoryId : QString(), items});
    }
    beginResetModel();
    const std::vector<Row> old = std::move(m_rows);
    m_rows = std::move(rows);
    endResetModel();
    // The view destroys its delegates after the reset: the inner lists may still be bound to these for a turn
    for (const Row &row : old)
        row.items->deleteLater();
    if (old.size() != m_rows.size())
        Q_EMIT countChanged();
}

void RecommendationRowsModel::clear()
{
    if (!m_rows.empty())
        setCategories({});
}

int RecommendationRowsModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : count();
}

QHash<int, QByteArray> RecommendationRowsModel::roleNames() const
{
    return {{KindRole, "kind"},
            {BaselineNameRole, "baselineName"},
            {CategoryIdRole, "categoryId"},
            {ItemsRole, "items"},
            {CountRole, "count"}};
}

QVariant RecommendationRowsModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.parent().isValid() || index.row() < 0 || index.row() >= count())
        return {};
    const Row &row = m_rows[size_t(index.row())];
    switch (role) {
    case KindRole:
        return row.kind;
    case Qt::DisplayRole:
    case BaselineNameRole:
        return row.baselineName;
    case CategoryIdRole:
        return row.categoryId;
    case ItemsRole:
        return QVariant::fromValue<QObject *>(row.items);
    case CountRole:
        return row.items->count();
    default:
        return {};
    }
}
} // namespace Kinefin
