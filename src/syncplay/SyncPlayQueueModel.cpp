// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "syncplay/SyncPlayQueueModel.h"

#include "api/JellyfinClient.h"

namespace Kinefin {
namespace {

// The most ids one read asks for
constexpr int ReadBatch = 100;

} // namespace

SyncPlayQueueModel::SyncPlayQueueModel(JellyfinClient *client, QObject *parent)
    : QAbstractListModel(parent), m_client(client)
{
    connect(m_client, &JellyfinClient::itemsReady, this,
            [this](const QJsonArray &items, int, int channel) { onItems(items, channel); });
    // A failed read leaves those rows nameless; the next queue reads what is still missing
    connect(m_client, &JellyfinClient::listingFailed, this,
            [this](const QString &, int, const QString &, int channel) {
                if (channel == Channel)
                    m_reading.clear();
            });
    connect(m_client, &JellyfinClient::credentialsCleared, this, &SyncPlayQueueModel::clear);
}

SyncPlayQueueModel::~SyncPlayQueueModel() = default;

int SyncPlayQueueModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_entries.size());
}

QVariant SyncPlayQueueModel::data(const QModelIndex &index, int role) const
{
    if (!checkIndex(index, CheckIndexOption::IndexIsValid | CheckIndexOption::ParentIsInvalid))
        return {};
    const SyncPlay::QueueEntry &entry = m_entries.at(index.row());
    const ItemSummary item = m_items.value(entry.itemId);
    switch (role) {
    case PlaylistItemIdRole:
        return entry.playlistItemId;
    case ItemIdRole:
        return entry.itemId;
    case NameRole:
        return item.name;
    case SeriesNameRole:
        return item.seriesName;
    case SeasonNumberRole:
        return item.valid ? item.seasonNumber : -1;
    case EpisodeNumberRole:
        return item.valid ? item.episodeNumber : -1;
    case CurrentRole:
        return index.row() == m_current;
    default:
        return {};
    }
}

QHash<int, QByteArray> SyncPlayQueueModel::roleNames() const
{
    return {{PlaylistItemIdRole, "playlistItemId"},
            {ItemIdRole, "itemId"},
            {NameRole, "name"},
            {SeriesNameRole, "seriesName"},
            {SeasonNumberRole, "seasonNumber"},
            {EpisodeNumberRole, "episodeNumber"},
            {CurrentRole, "current"}};
}

void SyncPlayQueueModel::setQueue(const QList<SyncPlay::QueueEntry> &entries, int playingIndex)
{
    bool sameRows = entries.size() == m_entries.size();
    for (qsizetype i = 0; sameRows && i < entries.size(); ++i)
        sameRows = entries.at(i).playlistItemId == m_entries.at(i).playlistItemId;
    const int before = m_current;
    if (sameRows) {
        // Only the playing entry moved: the rows stay, so a focused button in one survives
        m_current = playingIndex;
        if (before >= 0 && before < m_entries.size())
            Q_EMIT dataChanged(index(before), index(before), {CurrentRole});
        if (m_current >= 0 && m_current < m_entries.size())
            Q_EMIT dataChanged(index(m_current), index(m_current), {CurrentRole});
    } else {
        const bool countChanges = entries.size() != m_entries.size();
        beginResetModel();
        m_entries = entries;
        m_current = playingIndex;
        endResetModel();
        if (countChanges)
            Q_EMIT countChanged();
    }
    if (m_current != before)
        Q_EMIT currentIndexChanged();
    readNames();
}

void SyncPlayQueueModel::clear()
{
    if (!m_reading.isEmpty()) {
        m_reading.clear();
        m_client->releaseListingChannel(Channel);
    }
    m_items.clear();
    setQueue({}, -1);
}

void SyncPlayQueueModel::readNames()
{
    if (!m_reading.isEmpty())
        return;
    for (const SyncPlay::QueueEntry &entry : std::as_const(m_entries)) {
        if (!m_items.contains(entry.itemId) && !m_reading.contains(entry.itemId))
            m_reading.append(entry.itemId);
        if (m_reading.size() == ReadBatch)
            break;
    }
    if (m_reading.isEmpty())
        return;
    ItemQuery query;
    query.itemIds = m_reading;
    if (!m_client->fetchItems(query, Channel))
        m_reading.clear();
}

void SyncPlayQueueModel::onItems(const QJsonArray &items, int channel)
{
    if (channel != Channel || m_reading.isEmpty())
        return;
    for (const QJsonValue &value : items) {
        const ItemSummary item = parseItemSummary(value.toObject());
        if (item.valid && m_reading.contains(item.id.toLower()))
            m_items.insert(item.id.toLower(), item);
    }
    // An item the server did not return is not asked for again: it keeps no name
    for (const QString &id : std::as_const(m_reading)) {
        if (!m_items.contains(id))
            m_items.insert(id, ItemSummary{});
    }
    m_reading.clear();
    if (!m_entries.isEmpty())
        Q_EMIT dataChanged(index(0), index(int(m_entries.size()) - 1),
                           {NameRole, SeriesNameRole, SeasonNumberRole, EpisodeNumberRole});
    readNames();
}

} // namespace Kinefin
