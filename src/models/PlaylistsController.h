// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "models/ItemRowModel.h"

#include <QObject>
#include <QPointer>

namespace Kinefin {
class JellyfinClient;

// The video playlists of the signed-in user, sorted by name: at most ItemRowModel::MaxItems of them (`total` says how many the server holds). Read when the Playlists page asks, on its listing channel; the rows stay until the next read or the end of the session
class PlaylistsController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(Kinefin::ItemRowModel *list READ list CONSTANT)
    Q_PROPERTY(Status status READ status NOTIFY statusChanged)
    Q_PROPERTY(int total READ total NOTIFY statusChanged)
    // More playlists than the list holds
    Q_PROPERTY(bool capped READ capped NOTIFY statusChanged)

  public:
    enum Status { Idle, Loading, Ready, Failed };
    Q_ENUM(Status)
    static constexpr int Channel = 5120;

    explicit PlaylistsController(JellyfinClient *client, QObject *parent = nullptr);

    ItemRowModel *list() { return &m_list; }
    Status status() const { return m_status; }
    int total() const { return m_total; }
    bool capped() const
    {
        return m_total > m_list.count() && m_list.count() >= ItemRowModel::MaxItems;
    }

    // Reads the list again; the rows already shown stay until the answer
    Q_INVOKABLE void reload();
    // A read in flight is dropped and the channel released; the rows stay
    Q_INVOKABLE void close();
    // The name of a listed playlist, empty when it is not listed
    Q_INVOKABLE QString nameOf(const QString &playlistId) const;

  Q_SIGNALS:
    void statusChanged();

  private:
    void setStatus(Status status);

    QPointer<JellyfinClient> m_client;
    ItemRowModel m_list;
    // The answer the rows show: the same answer again keeps them (and the grid's place) as they are
    QJsonArray m_shown;
    Status m_status = Idle;
    int m_total = 0;
};
} // namespace Kinefin
