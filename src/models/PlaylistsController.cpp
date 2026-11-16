// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "PlaylistsController.h"

#include "models/ItemSummary.h"

#include <QJsonObject>

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
            [this](const QString &, int httpStatus, const QString &, int channel) {
                if (channel == Channel && m_status == Loading)
                    setStatus(Failed);
                else if (channel == CheckChannel && m_check)
                    failCheck(httpStatus);
            });
    connect(m_client, &JellyfinClient::playlistEntriesReady, this,
            [this](const QJsonArray &items, int total, int channel) {
                if (channel == CheckChannel && m_check)
                    onCheckEntries(items, total);
            });
    connect(
        m_client, &JellyfinClient::playlistWriteSucceeded, this,
        [this](JellyfinClient::PlaylistOp op, const QString &playlistId, const QString &createdId) {
            const std::optional<Write> write = takeWrite(op, playlistId);
            if (!write)
                return;
            if (m_open)
                reload();
            switch (op) {
            case JellyfinClient::PlaylistOp::Rename:
                Q_EMIT renamed(write->playlistId, write->name);
                break;
            case JellyfinClient::PlaylistOp::Delete:
                Q_EMIT deleted(write->playlistId);
                break;
            case JellyfinClient::PlaylistOp::Create:
                Q_EMIT created(createdId, write->withItems);
                break;
            case JellyfinClient::PlaylistOp::Add:
                Q_EMIT added(write->playlistId);
                break;
            default:
                break;
            }
        });
    connect(m_client, &JellyfinClient::playlistWriteFailed, this,
            [this](JellyfinClient::PlaylistOp op, const QString &playlistId, int httpStatus) {
                const std::optional<Write> write = takeWrite(op, playlistId);
                if (!write)
                    return;
                // A failure may still have been applied (a transport error): the list shows what the server holds
                if (m_open)
                    reload();
                const bool forbidden = httpStatus == 403;
                switch (op) {
                case JellyfinClient::PlaylistOp::Rename:
                    Q_EMIT renameFailed(write->playlistId, forbidden);
                    break;
                case JellyfinClient::PlaylistOp::Delete:
                    Q_EMIT deleteFailed(write->playlistId, forbidden);
                    break;
                case JellyfinClient::PlaylistOp::Create:
                    Q_EMIT createFailed(forbidden);
                    break;
                case JellyfinClient::PlaylistOp::Add:
                    Q_EMIT addFailed(write->playlistId, forbidden);
                    break;
                default:
                    break;
                }
            });
    // The client drops the writes and reads in flight with the session, with no answer
    connect(m_client, &JellyfinClient::credentialsCleared, this, [this] {
        m_shown = {};
        m_list.setItems({});
        m_total = 0;
        m_writes.clear();
        m_check.reset();
        updateWriting();
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
    updateWriting();
    return true;
}

bool PlaylistsController::deletePlaylist(const QString &playlistId)
{
    if (!m_client || !m_client->deletePlaylist(playlistId))
        return false;
    m_writes.push_back({JellyfinClient::PlaylistOp::Delete, playlistId, {}});
    updateWriting();
    return true;
}

bool PlaylistsController::create(const QString &name, const QStringList &itemIds)
{
    const QString normalized = normalizedPlaylistName(name);
    if (!m_client || !m_client->createPlaylist(normalized, itemIds))
        return false;
    m_writes.push_back({JellyfinClient::PlaylistOp::Create, {}, normalized, !itemIds.isEmpty()});
    updateWriting();
    return true;
}

bool PlaylistsController::add(const QString &playlistId, const QString &itemId)
{
    const bool adding =
        m_check || std::any_of(m_writes.cbegin(), m_writes.cend(), [](const Write &write) {
            return write.op == JellyfinClient::PlaylistOp::Add;
        });
    if (adding || !m_client || !m_client->isAuthenticated() || !isValidJellyfinId(playlistId)
        || !isValidJellyfinId(itemId))
        return false;
    m_check = Check{playlistId, itemId};
    if (!requestCheckPage()) {
        m_check.reset();
        return false;
    }
    updateWriting();
    return true;
}

bool PlaylistsController::requestCheckPage()
{
    return m_client->fetchPlaylistEntries(m_check->playlistId, m_check->received, CheckPageSize,
                                          CheckChannel);
}

void PlaylistsController::onCheckEntries(const QJsonArray &items, int total)
{
    const bool present = std::any_of(items.cbegin(), items.cend(), [this](const QJsonValue &item) {
        return sameItemId(item.toObject().value(QStringLiteral("Id")).toString(), m_check->itemId);
    });
    if (present) {
        const QString playlistId = m_check->playlistId;
        m_check.reset();
        updateWriting();
        Q_EMIT alreadyInPlaylist(playlistId);
        return;
    }
    m_check->received += int(items.size());
    // The server's own end ends the read, so do the cap and too many pages (a server that lies about its total)
    if (items.isEmpty() || m_check->received >= total || m_check->received >= MaxCheckedEntries
        || ++m_check->pages >= MaxCheckPages)
        return sendAdd();
    if (!requestCheckPage())
        failCheck(0);
}

void PlaylistsController::sendAdd()
{
    const Check check = *m_check;
    m_check.reset();
    if (!m_client->addToPlaylist(check.playlistId, {check.itemId})) {
        updateWriting();
        Q_EMIT addBusy(check.playlistId);
        return;
    }
    m_writes.push_back({JellyfinClient::PlaylistOp::Add, check.playlistId, {}});
    updateWriting();
}

void PlaylistsController::failCheck(int httpStatus)
{
    const QString playlistId = m_check ? m_check->playlistId : QString();
    m_check.reset();
    updateWriting();
    Q_EMIT addFailed(playlistId, httpStatus == 403);
}

void PlaylistsController::updateWriting()
{
    const bool now = writing();
    if (now == m_wasWriting)
        return;
    m_wasWriting = now;
    Q_EMIT writingChanged();
}

std::optional<PlaylistsController::Write>
PlaylistsController::takeWrite(JellyfinClient::PlaylistOp op, const QString &playlistId)
{
    const auto found = std::find_if(m_writes.begin(), m_writes.end(), [&](const Write &write) {
        return write.op == op
               && (op == JellyfinClient::PlaylistOp::Create
                   || sameItemId(write.playlistId, playlistId));
    });
    if (found == m_writes.end())
        return std::nullopt;
    Write write = std::move(*found);
    m_writes.erase(found);
    updateWriting();
    return write;
}

void PlaylistsController::setStatus(Status status)
{
    m_status = status;
    Q_EMIT statusChanged();
}
} // namespace Kinefin
