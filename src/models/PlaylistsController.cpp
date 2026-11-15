// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "PlaylistsController.h"

#include "api/JellyfinClient.h"
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
    connect(m_client, &JellyfinClient::credentialsCleared, this, [this] {
        m_shown = {};
        m_list.setItems({});
        m_total = 0;
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
    if (!m_client || !m_client->fetchItems(query, Channel)) {
        setStatus(Failed);
        return;
    }
    setStatus(Loading);
}

void PlaylistsController::close()
{
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

void PlaylistsController::setStatus(Status status)
{
    m_status = status;
    Q_EMIT statusChanged();
}
} // namespace Kinefin
