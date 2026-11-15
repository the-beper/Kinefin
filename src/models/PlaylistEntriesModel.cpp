// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "PlaylistEntriesModel.h"

#include "api/JellyfinClient.h"
#include "api/MediaSourceSelection.h"
#include "models/PlayQueue.h"

#include <QJsonObject>

#include <algorithm>
#include <utility>

namespace Kinefin {
PlaylistEntriesModel::PlaylistEntriesModel(JellyfinClient *client, QObject *parent)
    : QAbstractListModel(parent), m_client(client)
{
    if (!m_client)
        return;
    connect(m_client, &JellyfinClient::playlistEntriesReady, this,
            &PlaylistEntriesModel::onEntries);
    connect(m_client, &JellyfinClient::listingFailed, this,
            [this](const QString &, int, const QString &, int channel) {
                if (channel != Channel || m_status != Loading)
                    return;
                m_pending.clear();
                m_status = Failed;
                Q_EMIT statusChanged();
            });
    connect(m_client, &JellyfinClient::credentialsCleared, this, [this] {
        m_playlistId.clear();
        m_pending.clear();
        m_total = 0;
        m_capped = false;
        m_status = Idle;
        reset();
        Q_EMIT statusChanged();
    });
}

void PlaylistEntriesModel::load(const QString &playlistId)
{
    if (!isValidJellyfinId(playlistId))
        return;
    if (!sameItemId(playlistId, m_playlistId)) {
        m_playlistId = playlistId;
        m_total = 0;
        m_capped = false;
        reset();
    }
    m_pending.clear();
    m_received = 0;
    m_pages = 0;
    m_pageStart = 0;
    m_status = Loading;
    requestPage();
    Q_EMIT statusChanged();
}

void PlaylistEntriesModel::close()
{
    if (m_status != Loading)
        return;
    if (m_client)
        m_client->releaseListingChannel(Channel);
    m_pending.clear();
    m_status = Idle;
    Q_EMIT statusChanged();
}

void PlaylistEntriesModel::requestPage()
{
    if (!m_client || !m_client->fetchPlaylistEntries(m_playlistId, m_received, PageSize, Channel)) {
        m_pending.clear();
        m_status = Failed;
    }
}

void PlaylistEntriesModel::onEntries(const QJsonArray &items, int total, int channel)
{
    if (channel != Channel || m_status != Loading)
        return;
    for (const QJsonValue &value : items) {
        if (m_pending.size() >= size_t(MaxEntries))
            break;
        const QJsonObject object = value.toObject();
        const ItemSummary item = parseItemSummary(object);
        if (!item.valid)
            continue;
        const QString entryId = object.value(QStringLiteral("PlaylistItemId")).toString();
        m_pending.push_back({item, isValidJellyfinId(entryId) ? entryId : QString()});
    }
    m_received += int(items.size());
    m_total = total;
    m_capped = m_pending.size() >= size_t(MaxEntries) && total > MaxEntries;
    // The server's own end ends the read, so do the cap, a page that adds no row and too many pages (a server that lies about its total)
    if (items.isEmpty() || m_received >= total || m_pending.size() >= size_t(MaxEntries)
        || ++m_pages >= MaxPages || m_pending.size() == m_pageStart)
        return publish();
    m_pageStart = m_pending.size();
    requestPage();
    if (m_status == Failed)
        Q_EMIT statusChanged();
}

// What a row shows and acts on: the same in both reads keeps the row
bool PlaylistEntriesModel::sameEntry(const Entry &a, const Entry &b)
{
    return a.entryId == b.entryId && a.item.id == b.item.id && a.item.name == b.item.name
           && a.item.seriesName == b.item.seriesName && a.item.seasonNumber == b.item.seasonNumber
           && a.item.episodeNumber == b.item.episodeNumber && a.item.year == b.item.year
           && a.item.runtimeMinutes == b.item.runtimeMinutes && a.item.type == b.item.type
           && a.item.played == b.item.played && a.item.primaryTag == b.item.primaryTag
           && a.item.parentBackdropItemId == b.item.parentBackdropItemId
           && a.item.parentBackdropTag == b.item.parentBackdropTag;
}

void PlaylistEntriesModel::publish()
{
    if (!std::equal(m_pending.cbegin(), m_pending.cend(), m_rows.cbegin(), m_rows.cend(),
                    &PlaylistEntriesModel::sameEntry)) {
        beginResetModel();
        m_rows = std::exchange(m_pending, {});
        endResetModel();
    }
    m_pending.clear();
    m_status = Ready;
    Q_EMIT statusChanged();
}

void PlaylistEntriesModel::reset()
{
    beginResetModel();
    m_rows.clear();
    endResetModel();
}

int PlaylistEntriesModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : count();
}

QVariant PlaylistEntriesModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.parent().isValid() || index.row() < 0 || index.row() >= count())
        return {};
    const Entry &entry = m_rows[size_t(index.row())];
    const ItemSummary &item = entry.item;
    switch (role) {
    case Qt::DisplayRole:
    case NameRole:
        return item.name;
    case ItemIdRole:
        return item.id;
    case SeriesNameRole:
        return item.seriesName;
    case SeasonNumberRole:
        return item.seasonNumber;
    case EpisodeNumberRole:
        return item.episodeNumber;
    case YearRole:
        return item.year;
    case RuntimeMinutesRole:
        return item.runtimeMinutes;
    case TypeRole:
        return item.type;
    case ImageUrlRole:
        return listImageUrl(m_client, item, ImageWidth);
    case PlayedRole:
        return item.played;
    case EntryIdRole:
        return entry.entryId;
    default:
        return {};
    }
}

QHash<int, QByteArray> PlaylistEntriesModel::roleNames() const
{
    return {{ItemIdRole, "itemId"},
            {NameRole, "name"},
            {SeriesNameRole, "seriesName"},
            {SeasonNumberRole, "seasonNumber"},
            {EpisodeNumberRole, "episodeNumber"},
            {YearRole, "year"},
            {RuntimeMinutesRole, "runtimeMinutes"},
            {TypeRole, "type"},
            {ImageUrlRole, "imageUrl"},
            {PlayedRole, "played"},
            {EntryIdRole, "entryId"}};
}
} // namespace Kinefin
