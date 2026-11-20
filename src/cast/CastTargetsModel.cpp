// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "cast/CastTargetsModel.h"

#include <utility>

namespace Kinefin {

CastTargetsModel::CastTargetsModel(QObject *parent) : QAbstractListModel(parent) {}

int CastTargetsModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_targets.size());
}

QVariant CastTargetsModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_targets.size())
        return {};
    const Target &target = m_targets.at(index.row());
    switch (role) {
    case UdnRole:
        return target.udn;
    case NameRole:
        return target.name;
    case DetailRole:
        return target.detail;
    case KindRole:
        return target.kind;
    default:
        return {};
    }
}

QHash<int, QByteArray> CastTargetsModel::roleNames() const
{
    return {
        {UdnRole, "udn"},
        {NameRole, "name"},
        {DetailRole, "detail"},
        {KindRole, "kind"},
    };
}

void CastTargetsModel::add(Target target)
{
    if (const int row = indexOf(target.udn); row >= 0) {
        m_targets[row] = std::move(target);
        Q_EMIT dataChanged(index(row), index(row));
        return;
    }
    beginInsertRows({}, static_cast<int>(m_targets.size()), static_cast<int>(m_targets.size()));
    m_targets.append(std::move(target));
    endInsertRows();
}

void CastTargetsModel::remove(const QString &udn)
{
    const int row = indexOf(udn);
    if (row < 0)
        return;
    beginRemoveRows({}, row, row);
    m_targets.removeAt(row);
    endRemoveRows();
}

QStringList CastTargetsModel::udns() const
{
    QStringList list;
    list.reserve(m_targets.size());
    for (const Target &target : m_targets)
        list.append(target.udn);
    return list;
}

int CastTargetsModel::indexOf(const QString &udn) const
{
    for (int row = 0; row < m_targets.size(); ++row) {
        if (m_targets.at(row).udn == udn)
            return row;
    }
    return -1;
}

} // namespace Kinefin
