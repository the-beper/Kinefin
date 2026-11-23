// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "PlaylistEntriesModel.h"

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
                m_trusted = false;
                m_status = Failed;
                Q_EMIT statusChanged();
            });
    connect(m_client, &JellyfinClient::playlistWriteSucceeded, this,
            [this](JellyfinClient::PlaylistOp op, const QString &playlistId, const QString &) {
                onWriteSucceeded(op, playlistId);
            });
    connect(m_client, &JellyfinClient::playlistWriteFailed, this,
            &PlaylistEntriesModel::onWriteFailed);
    // The check of a row follows what plays: the list is not read again after a playback
    connect(m_client, &JellyfinClient::itemPlayedUpdated, this,
            [this](const QString &id, bool played) {
                for (size_t i = 0; i < m_rows.size(); ++i) {
                    if (!sameItemId(m_rows[i].item.id, id))
                        continue;
                    applyPlayed(m_rows[i].item, played);
                    const QModelIndex row = index(int(i));
                    Q_EMIT dataChanged(row, row, {PlayedRole});
                }
            });
    connect(m_client, &JellyfinClient::itemProgressUpdated, this,
            [this](const QString &id, qint64 position, double percentage) {
                for (size_t i = 0; i < m_rows.size(); ++i) {
                    if (!sameItemId(m_rows[i].item.id, id))
                        continue;
                    applyProgress(m_rows[i].item, position, percentage);
                    const QModelIndex row = index(int(i));
                    Q_EMIT dataChanged(row, row, {PlayedRole});
                }
            });
    // The client drops the writes in flight with the session, with no answer
    connect(m_client, &JellyfinClient::credentialsCleared, this, [this] {
        m_playlistId.clear();
        m_pending.clear();
        m_total = 0;
        m_capped = false;
        m_gap = false;
        m_trusted = false;
        m_sent.reset();
        m_waiting.clear();
        m_readAfterWrites = false;
        m_status = Idle;
        reset();
        setEditError(NoEditError);
        updateWriting();
        Q_EMIT statusChanged();
    });
}

bool PlaylistEntriesModel::writing() const
{
    const auto ofThisPlaylist = [this](const Write &write) {
        return sameItemId(write.playlistId, m_playlistId);
    };
    return (m_sent && ofThisPlaylist(*m_sent))
           || std::any_of(m_waiting.cbegin(), m_waiting.cend(), ofThisPlaylist);
}

void PlaylistEntriesModel::load(const QString &playlistId)
{
    if (!isValidJellyfinId(playlistId))
        return;
    m_open = true;
    const bool switched = !sameItemId(playlistId, m_playlistId);
    if (switched) {
        m_playlistId = playlistId;
        m_total = 0;
        m_capped = false;
        m_gap = false;
        m_trusted = false;
        m_readAfterWrites = false;
        reset();
        setEditError(NoEditError);
        updateWriting();
    }
    if (writing()) {
        // A read now could bring back the order before the edits on their way (also of a playlist shown again while one of its writes is still out): it waits for the last of them
        if (m_status == Loading && m_client)
            m_client->releaseListingChannel(Channel);
        m_pending.clear();
        m_readAfterWrites = true;
        if (switched) {
            m_status = Loading;
            Q_EMIT statusChanged();
        }
        return;
    }
    m_pending.clear();
    m_pendingGap = false;
    m_received = 0;
    m_pages = 0;
    m_pageStart = 0;
    m_status = Loading;
    requestPage();
    Q_EMIT statusChanged();
}

void PlaylistEntriesModel::close()
{
    m_open = false;
    m_readAfterWrites = false;
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
        if (!item.valid) {
            m_pendingGap = true;
            continue;
        }
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
    m_gap = m_pendingGap;
    m_trusted = true;
    m_status = Ready;
    Q_EMIT statusChanged();
}

bool PlaylistEntriesModel::canEdit() const
{
    return m_client && m_client->isAuthenticated() && m_trusted
           && (m_status == Ready || m_status == Loading)
           && m_waiting.size() < size_t(MaxWaitingWrites);
}

bool PlaylistEntriesModel::canMove(int row) const
{
    if (!canEdit() || m_gap || row < 0 || row >= count())
        return false;
    const QString &entryId = m_rows[size_t(row)].entryId;
    return !entryId.isEmpty()
           && std::count_if(m_rows.cbegin(), m_rows.cend(), [&entryId](const Entry &entry) {
                  return sameItemId(entry.entryId, entryId);
              }) == 1;
}

bool PlaylistEntriesModel::canRemove(int row) const
{
    return canEdit() && row >= 0 && row < count() && !m_rows[size_t(row)].entryId.isEmpty();
}

bool PlaylistEntriesModel::move(int from, int to)
{
    if (!canMove(from) || to < 0 || to >= count() || to == from)
        return false;
    beginMoveRows(QModelIndex(), from, from, QModelIndex(), to > from ? to + 1 : to);
    Entry entry = std::move(m_rows[size_t(from)]);
    m_rows.erase(m_rows.begin() + from);
    m_rows.insert(m_rows.begin() + to, std::move(entry));
    endMoveRows();
    // The rows are the server's from the first, so a row's index is its place there: the server takes the place the entry ends at
    enqueue({JellyfinClient::PlaylistOp::Move, m_playlistId, m_rows[size_t(to)].entryId, to});
    return true;
}

bool PlaylistEntriesModel::remove(int row)
{
    if (!canRemove(row))
        return false;
    const QString entryId = m_rows[size_t(row)].entryId;
    int removed = 0;
    for (int index = count() - 1; index >= 0; --index) {
        if (!sameItemId(m_rows[size_t(index)].entryId, entryId))
            continue;
        beginRemoveRows(QModelIndex(), index, index);
        m_rows.erase(m_rows.begin() + index);
        endRemoveRows();
        ++removed;
    }
    m_total = std::max(0, m_total - removed);
    m_capped = m_capped && m_total > count();
    enqueue({JellyfinClient::PlaylistOp::Remove, m_playlistId, entryId, -1});
    Q_EMIT statusChanged();
    return true;
}

void PlaylistEntriesModel::enqueue(Write write)
{
    // A read on its way would bring back the order before the edit: it is asked again after the last one
    if (m_status == Loading) {
        if (m_client)
            m_client->releaseListingChannel(Channel);
        m_pending.clear();
        m_status = Ready;
        Q_EMIT statusChanged();
    }
    m_readAfterWrites = true;
    m_waiting.push_back(std::move(write));
    setEditError(NoEditError);
    if (!m_sent)
        sendNext();
    updateWriting();
    Q_EMIT edited();
}

void PlaylistEntriesModel::sendNext()
{
    if (m_sent || m_waiting.empty())
        return;
    m_sent = m_waiting.front();
    m_waiting.pop_front();
    const Write write = *m_sent;
    const bool sent =
        m_client
        && (write.op == JellyfinClient::PlaylistOp::Move
                ? m_client->movePlaylistEntry(write.playlistId, write.entryId, write.newIndex)
                : m_client->removePlaylistEntries(write.playlistId, {write.entryId}));
    // The client refused it (another write of the playlist on its way, the session gone): the edits of the playlist stop as after a failure
    if (!sent)
        onWriteFailed(write.op, write.playlistId, 0);
}

bool PlaylistEntriesModel::isOwnWrite(JellyfinClient::PlaylistOp op,
                                      const QString &playlistId) const
{
    return m_sent && m_sent->op == op && sameItemId(playlistId, m_sent->playlistId);
}

void PlaylistEntriesModel::onWriteSucceeded(JellyfinClient::PlaylistOp op,
                                            const QString &playlistId)
{
    if (!isOwnWrite(op, playlistId))
        return;
    m_sent.reset();
    sendNext();
    updateWriting();
    if (!writing() && m_readAfterWrites) {
        m_readAfterWrites = false;
        load(m_playlistId);
    }
}

void PlaylistEntriesModel::onWriteFailed(JellyfinClient::PlaylistOp op, const QString &playlistId,
                                         int httpStatus)
{
    if (!isOwnWrite(op, playlistId))
        return;
    const QString failed = m_sent->playlistId;
    m_sent.reset();
    // The edits of that playlist that wait were made over the order the failure leaves unknown
    std::erase_if(m_waiting,
                  [&failed](const Write &write) { return sameItemId(write.playlistId, failed); });
    if (!sameItemId(failed, m_playlistId)) {
        // A playlist no page shows any more: its next visit reads it again
        sendNext();
        updateWriting();
        return;
    }
    m_readAfterWrites = false;
    m_trusted = false;
    sendNext();
    updateWriting();
    setEditError(httpStatus == 403 ? EditForbidden : EditFailed);
    // Without a page the rows stay untrusted, and the next visit reads them
    if (m_open)
        load(m_playlistId);
}

void PlaylistEntriesModel::setEditError(EditError error)
{
    if (m_editError == error)
        return;
    m_editError = error;
    Q_EMIT editErrorChanged();
}

void PlaylistEntriesModel::updateWriting()
{
    const bool now = writing();
    if (now == m_wasWriting)
        return;
    m_wasWriting = now;
    Q_EMIT writingChanged();
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
        return watchedToEnd(item);
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
