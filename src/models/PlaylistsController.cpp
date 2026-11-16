// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "PlaylistsController.h"

#include "models/ItemSummary.h"

#include <algorithm>

namespace Kinefin {
PlaylistsController::PlaylistsController(JellyfinClient *client, QObject *parent)
    : QObject(parent), m_client(client), m_list(client, false, this)
{
    if (!m_client)
        return;
    connect(m_client, &JellyfinClient::itemsReady, this,
            [this](const QJsonArray &items, int total, int channel) {
                if (channel != Channel || m_status != Loading)
                    return;
                if (items != m_shown) {
                    m_shown = items;
                    m_list.setItems(items);
                }
                m_total = std::max(total, m_list.count());
                setStatus(Ready);
            });
    connect(m_client, &JellyfinClient::listingFailed, this,
            [this](const QString &, int, const QString &, int channel) {
                if (channel == Channel && m_status == Loading)
                    setStatus(Failed);
            });
    connect(m_client, &JellyfinClient::playlistWriteSucceeded, this,
            [this](JellyfinClient::PlaylistOp op, const QString &playlistId, const QString &) {
                const std::optional<Write> write = takeWrite(op, playlistId);
                if (!write)
                    return;
                if (m_open)
                    reload();
                if (op == JellyfinClient::PlaylistOp::Rename)
                    Q_EMIT renamed(write->playlistId, write->name);
                else
                    Q_EMIT deleted(write->playlistId);
            });
    connect(m_client, &JellyfinClient::playlistWriteFailed, this,
            [this](JellyfinClient::PlaylistOp op, const QString &playlistId, int httpStatus) {
                const std::optional<Write> write = takeWrite(op, playlistId);
                if (!write)
                    return;
                // A failure may still have been applied (a transport error): the list shows what the server holds
                if (m_open)
                    reload();
                if (op == JellyfinClient::PlaylistOp::Rename)
                    Q_EMIT renameFailed(write->playlistId, httpStatus == 403);
                else
                    Q_EMIT deleteFailed(write->playlistId, httpStatus == 403);
            });
    // The client drops the writes in flight with the session, with no answer
    connect(m_client, &JellyfinClient::credentialsCleared, this, [this] {
        m_shown = {};
        m_list.setItems({});
        m_total = 0;
        const bool wasWriting = writing();
        m_writes.clear();
        if (wasWriting)
            Q_EMIT writingChanged();
        setStatus(Idle);
    });
}

void PlaylistsController::reload()
{
    // Video playlists only while music is out of scope; the server sorts them by name
    ItemQuery query;
    query.includeItemTypes = QStringLiteral("Playlist");
    query.mediaTypes = QStringLiteral("Video");
    query.recursive = true;
    query.sortBy = QStringLiteral("SortName");
    query.limit = ItemRowModel::MaxItems;
    query.cardImages = true;
    m_open = true;
    if (!m_client || !m_client->fetchItems(query, Channel)) {
        setStatus(Failed);
        return;
    }
    setStatus(Loading);
}

void PlaylistsController::close()
{
    m_open = false;
    if (m_status != Loading)
        return;
    if (m_client)
        m_client->releaseListingChannel(Channel);
    setStatus(Idle);
}

QString PlaylistsController::nameOf(const QString &playlistId) const
{
    for (int row = 0; row < m_list.count(); ++row) {
        const QModelIndex index = m_list.index(row);
        if (sameItemId(index.data(ItemRowModel::ItemIdRole).toString(), playlistId))
            return index.data(ItemRowModel::NameRole).toString();
    }
    return {};
}

QString PlaylistsController::validName(const QString &name) const
{
    return normalizedPlaylistName(name);
}

bool PlaylistsController::rename(const QString &playlistId, const QString &name)
{
    const QString normalized = normalizedPlaylistName(name);
    if (!m_client || !m_client->renamePlaylist(playlistId, normalized))
        return false;
    m_writes.push_back({JellyfinClient::PlaylistOp::Rename, playlistId, normalized});
    if (m_writes.size() == 1)
        Q_EMIT writingChanged();
    return true;
}

bool PlaylistsController::deletePlaylist(const QString &playlistId)
{
    if (!m_client || !m_client->deletePlaylist(playlistId))
        return false;
    m_writes.push_back({JellyfinClient::PlaylistOp::Delete, playlistId, {}});
    if (m_writes.size() == 1)
        Q_EMIT writingChanged();
    return true;
}

std::optional<PlaylistsController::Write>
PlaylistsController::takeWrite(JellyfinClient::PlaylistOp op, const QString &playlistId)
{
    const auto found = std::find_if(m_writes.begin(), m_writes.end(), [&](const Write &write) {
        return write.op == op && sameItemId(write.playlistId, playlistId);
    });
    if (found == m_writes.end())
        return std::nullopt;
    Write write = std::move(*found);
    m_writes.erase(found);
    if (m_writes.empty())
        Q_EMIT writingChanged();
    return write;
}

void PlaylistsController::setStatus(Status status)
{
    m_status = status;
    Q_EMIT statusChanged();
}
} // namespace Kinefin
