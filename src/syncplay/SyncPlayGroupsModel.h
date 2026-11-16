// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/SyncPlayProtocol.h"

#include <QAbstractListModel>
#include <QList>

namespace Kinefin {
class JellyfinClient;

// The server's SyncPlay groups the user may join (`GET /SyncPlay/List`), for the SyncPlay dialog: read on reload(), kept until the next answer, emptied when the session ends. The names are remote text for plain-text labels only
class SyncPlayGroupsModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(Status status READ status NOTIFY statusChanged)
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

  public:
    enum class Status { Idle, Loading, Ready, Failed };
    Q_ENUM(Status)
    // The group's state as the server last listed it
    enum class GroupState { Idle, Waiting, Paused, Playing };
    Q_ENUM(GroupState)
    enum Role {
        GroupIdRole = Qt::UserRole + 1,
        NameRole,
        // The members' names, one per user, plain text
        MembersRole,
        StateRole
    };

    explicit SyncPlayGroupsModel(JellyfinClient *client, QObject *parent = nullptr);

    [[nodiscard]] Status status() const { return m_status; }
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Asks the server again; the rows stay until the answer. Does nothing when not signed in
    Q_INVOKABLE void reload();

  Q_SIGNALS:
    void statusChanged();
    void countChanged();

  private:
    void setStatus(Status status);
    void replace(const QList<SyncPlay::GroupInfo> &groups);

    JellyfinClient *m_client;
    QList<SyncPlay::GroupInfo> m_groups;
    Status m_status = Status::Idle;
};

} // namespace Kinefin
