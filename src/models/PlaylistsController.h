// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/JellyfinClient.h"
#include "models/ItemRowModel.h"

#include <QObject>
#include <QPointer>

#include <optional>
#include <vector>

namespace Kinefin {
// The video playlists of the signed-in user, sorted by name: at most ItemRowModel::MaxItems of them (`total` says how many the server holds). Read when the Playlists page asks, on its listing channel; the rows stay until the next read or the end of the session. Renames and deletes go through here, and the list is read again after each answer while a page shows it
class PlaylistsController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(Kinefin::ItemRowModel *list READ list CONSTANT)
    Q_PROPERTY(Status status READ status NOTIFY statusChanged)
    Q_PROPERTY(int total READ total NOTIFY statusChanged)
    // More playlists than the list holds
    Q_PROPERTY(bool capped READ capped NOTIFY statusChanged)
    // A rename or delete on its way
    Q_PROPERTY(bool writing READ writing NOTIFY writingChanged)
    // The longest name the server takes, in UTF-16 units for a text field (a code point is at least one)
    Q_PROPERTY(int maxNameLength READ maxNameLength CONSTANT)

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

    // Reads the list again; the rows already shown stay until the answer. Until close() the list is also read after each rename or delete answer
    Q_INVOKABLE void reload();
    // A read in flight is dropped and the channel released; the rows stay
    Q_INVOKABLE void close();
    // The name of a listed playlist, empty when it is not listed
    Q_INVOKABLE QString nameOf(const QString &playlistId) const;
    // The name `name` stands for as a playlist name (normalizedPlaylistName), empty when it does not qualify
    Q_INVOKABLE QString validName(const QString &name) const;

    bool writing() const { return !m_writes.empty(); }
    static int maxNameLength() { return MaxPlaylistNameChars; }
    // Sends the rename or the delete; false when refused (an invalid id or name, a write of the playlist already on its way, no session). The outcome comes as renamed or deleted, else as renameFailed or deleteFailed
    Q_INVOKABLE bool rename(const QString &playlistId, const QString &name);
    Q_INVOKABLE bool deletePlaylist(const QString &playlistId);

  Q_SIGNALS:
    void statusChanged();
    void writingChanged();
    // `name` is the name sent, as normalizedPlaylistName made it
    void renamed(const QString &playlistId, const QString &name);
    void deleted(const QString &playlistId);
    // `forbidden`: the server refused the user (403), the playlist is someone else's
    void renameFailed(const QString &playlistId, bool forbidden);
    void deleteFailed(const QString &playlistId, bool forbidden);

  private:
    struct Write {
        JellyfinClient::PlaylistOp op;
        QString playlistId;
        QString name;
    };
    void setStatus(Status status);
    // The write of `op` on `playlistId` this controller sent, taken off the list; empty when it sent none
    std::optional<Write> takeWrite(JellyfinClient::PlaylistOp op, const QString &playlistId);

    QPointer<JellyfinClient> m_client;
    ItemRowModel m_list;
    // The answer the rows show: the same answer again keeps them (and the grid's place) as they are
    QJsonArray m_shown;
    Status m_status = Idle;
    int m_total = 0;
    std::vector<Write> m_writes;
    // A page shows the list: between reload() and close()
    bool m_open = false;
};
} // namespace Kinefin
