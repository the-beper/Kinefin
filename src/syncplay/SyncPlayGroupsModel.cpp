// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "syncplay/SyncPlayGroupsModel.h"

#include "api/JellyfinClient.h"

namespace Kinefin {

SyncPlayGroupsModel::SyncPlayGroupsModel(JellyfinClient *client, QObject *parent)
    : QAbstractListModel(parent), m_client(client)
{
    connect(m_client, &JellyfinClient::syncPlayGroupsReady, this,
            [this](const QList<SyncPlay::GroupInfo> &groups) {
                if (m_status != Status::Loading)
                    return;
                replace(groups);
                setStatus(Status::Ready);
            });
    connect(m_client, &JellyfinClient::syncPlayGroupsFailed, this, [this] {
        if (m_status == Status::Loading)
            setStatus(Status::Failed);
    });
    connect(m_client, &JellyfinClient::credentialsCleared, this, [this] {
        replace({});
        setStatus(Status::Idle);
    });
}

int SyncPlayGroupsModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_groups.size());
}

QVariant SyncPlayGroupsModel::data(const QModelIndex &index, int role) const
{
    if (!checkIndex(index, CheckIndexOption::IndexIsValid | CheckIndexOption::ParentIsInvalid))
        return {};
    const SyncPlay::GroupInfo &group = m_groups.at(index.row());
    switch (role) {
    case GroupIdRole:
        return group.groupId;
    case NameRole:
        return group.name;
    case MembersRole:
        return group.participants;
    case StateRole:
        return int(group.state);
    default:
        return {};
    }
}

QHash<int, QByteArray> SyncPlayGroupsModel::roleNames() const
{
    return {{GroupIdRole, "groupId"},
            {NameRole, "name"},
            {MembersRole, "members"},
            {StateRole, "groupState"}};
}

void SyncPlayGroupsModel::reload()
{
    if (!m_client->fetchSyncPlayGroups())
        return;
    setStatus(Status::Loading);
}

void SyncPlayGroupsModel::setStatus(Status status)
{
    if (m_status == status)
        return;
    m_status = status;
    Q_EMIT statusChanged();
}

void SyncPlayGroupsModel::replace(const QList<SyncPlay::GroupInfo> &groups)
{
    const bool countChanges = groups.size() != m_groups.size();
    beginResetModel();
    m_groups = groups;
    endResetModel();
    if (countChanges)
        Q_EMIT countChanged();
}

} // namespace Kinefin
