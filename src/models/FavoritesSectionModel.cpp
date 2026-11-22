// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "FavoritesSectionModel.h"

namespace Kinefin {
FavoritesSectionModel::FavoritesSectionModel(QObject *parent) : QAbstractListModel(parent) {}

void FavoritesSectionModel::setSource(FavoritesModel *source)
{
    if (source == m_source)
        return;
    if (m_source)
        disconnect(m_source, nullptr, this, nullptr);
    m_source = source;
    if (m_source) {
        // The snapshot only ever changes by a reset: the rows are dropped before it and found again after
        connect(m_source, &QAbstractItemModel::modelAboutToBeReset, this, [this] {
            beginResetModel();
            m_rows.clear();
            m_total = 0;
        });
        connect(m_source, &QAbstractItemModel::modelReset, this, [this] {
            fill();
            endResetModel();
            Q_EMIT countChanged();
        });
        connect(m_source, &FavoritesModel::filtersChanged, this, [this] { refill(); });
        connect(m_source, &QObject::destroyed, this, [this] {
            m_source = nullptr;
            refill();
            Q_EMIT sourceChanged();
        });
    }
    refill();
    Q_EMIT sourceChanged();
}

void FavoritesSectionModel::setType(const QString &type)
{
    if (type == m_type)
        return;
    m_type = type;
    refill();
    Q_EMIT typeChanged();
}

void FavoritesSectionModel::setLimit(int limit)
{
    limit = qMax(limit, 0);
    if (limit == m_limit)
        return;
    m_limit = limit;
    refill();
    Q_EMIT limitChanged();
}

void FavoritesSectionModel::fill()
{
    m_rows.clear();
    m_total = 0;
    if (!m_source)
        return;
    for (int row = 0; row < m_source->count(); ++row) {
        if (m_source->itemAt(row).summary.type != m_type || !m_source->passesFilters(row))
            continue;
        ++m_total;
        if (m_limit == 0 || int(m_rows.size()) < m_limit)
            m_rows.push_back(row);
    }
}

void FavoritesSectionModel::refill()
{
    beginResetModel();
    fill();
    endResetModel();
    Q_EMIT countChanged();
}

int FavoritesSectionModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : count();
}

QVariant FavoritesSectionModel::data(const QModelIndex &index, int role) const
{
    if (!m_source || !index.isValid() || index.parent().isValid() || index.row() < 0
        || index.row() >= count())
        return {};
    return m_source->data(m_source->index(m_rows[size_t(index.row())], 0), role);
}

QHash<int, QByteArray> FavoritesSectionModel::roleNames() const
{
    return FavoritesModel::itemRoleNames();
}
} // namespace Kinefin
