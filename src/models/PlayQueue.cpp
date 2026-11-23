// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "PlayQueue.h"

#include "api/MediaSourceSelection.h"
#include "settings/ProfilePaths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QSet>

#include <algorithm>

namespace Kinefin {
namespace {
// The same item written with or without dashes, in any case, is one id
QString idKey(const QString &id)
{
    return id.toLower().remove(QLatin1Char('-'));
}

QByteArray queueFileBytes(const QString &serverId, const QString &userId, const QStringList &ids)
{
    const QJsonObject root{{QStringLiteral("version"), PlayQueue::FileVersion},
                           {QStringLiteral("serverId"), serverId},
                           {QStringLiteral("userId"), userId},
                           {QStringLiteral("items"), QJsonArray::fromStringList(ids)}};
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

bool sameId(const QString &a, const QString &b)
{
    return a.compare(b, Qt::CaseInsensitive) == 0;
}
} // namespace

PlayQueue::PlayQueue(JellyfinClient *client, QObject *parent)
    : QAbstractListModel(parent), m_client(client)
{
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(1000);
    connect(&m_saveTimer, &QTimer::timeout, this, &PlayQueue::flush);
    connect(this, &PlayQueue::edited, this, &PlayQueue::scheduleSave);
    if (!m_client)
        return;
    connect(m_client, &JellyfinClient::itemsReady, this, &PlayQueue::onItems);
    connect(m_client, &JellyfinClient::nextUpReady, this, &PlayQueue::onNextUp);
    connect(m_client, &JellyfinClient::episodesReady, this, &PlayQueue::onEpisodes);
    connect(m_client, &JellyfinClient::playlistEntriesReady, this, &PlayQueue::onPlaylistEntries);
    connect(m_client, &JellyfinClient::listingFailed, this, &PlayQueue::onListingFailed);
    // The check of a row follows what plays: a queue is not read again from the server, and an entry that was just watched must show as played (not a queue edit, so nothing is saved for it)
    connect(m_client, &JellyfinClient::itemPlayedUpdated, this,
            [this](const QString &id, bool played) {
                for (size_t i = 0; i < m_items.size(); ++i) {
                    if (!sameItemId(m_items[i].id, id))
                        continue;
                    applyPlayed(m_items[i], played);
                    const QModelIndex row = index(int(i));
                    Q_EMIT dataChanged(row, row, {PlayedRole});
                }
            });
    connect(m_client, &JellyfinClient::itemProgressUpdated, this,
            [this](const QString &id, qint64 position, double percentage) {
                for (size_t i = 0; i < m_items.size(); ++i) {
                    if (!sameItemId(m_items[i].id, id))
                        continue;
                    applyProgress(m_items[i], position, percentage);
                    const QModelIndex row = index(int(i));
                    Q_EMIT dataChanged(row, row, {PlayedRole});
                }
            });
    connect(m_client, &JellyfinClient::credentialsCleared, this, [this] {
        // What was edited is saved for the next sign-in of the same user; the stored file stays
        flush();
        endOperation();
        endRestore();
        dropMemory();
        m_serverId.clear();
        m_userId.clear();
    });
}

PlayQueue::~PlayQueue()
{
    flush();
    if (m_client) {
        m_client->releaseListingChannel(ExpandChannel);
        m_client->releaseListingChannel(RestoreChannel);
    }
}

int PlayQueue::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : count();
}

QHash<int, QByteArray> PlayQueue::roleNames() const
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
            {PlayedRole, "played"}};
}

QUrl listImageUrl(const JellyfinClient *client, const ItemSummary &item, int width)
{
    if (!client)
        return {};
    if (!item.primaryTag.isEmpty())
        return client->imageUrl(item.id, QStringLiteral("Primary"), width, item.primaryTag);
    if (!item.parentBackdropItemId.isEmpty() && !item.parentBackdropTag.isEmpty())
        return client->imageUrl(item.parentBackdropItemId, QStringLiteral("Backdrop"), width,
                                item.parentBackdropTag);
    return {};
}

int PlayQueue::indexOf(const QString &itemId) const
{
    const auto found = std::find_if(m_items.cbegin(), m_items.cend(), [&](const ItemSummary &item) {
        return idKey(item.id) == idKey(itemId);
    });
    return found == m_items.cend() ? -1 : int(std::distance(m_items.cbegin(), found));
}

QUrl PlayQueue::imageUrlFor(const ItemSummary &item) const
{
    return listImageUrl(m_client, item, ImageWidth);
}

QVariant PlayQueue::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.parent().isValid() || index.row() < 0 || index.row() >= count())
        return {};
    const ItemSummary &item = m_items[size_t(index.row())];
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
        return imageUrlFor(item);
    case PlayedRole:
        return watchedToEnd(item);
    default:
        return {};
    }
}

QStringList PlayQueue::itemIds() const
{
    QStringList ids;
    ids.reserve(count());
    for (const ItemSummary &item : m_items)
        ids.append(item.id);
    return ids;
}

void PlayQueue::setCurrentPlaybackItemId(const QString &id)
{
    m_currentPlaybackItemId = isValidJellyfinId(id) ? id.toLower() : QString();
}

bool PlayQueue::add(const QString &itemId, Placement placement)
{
    return beginOperation(itemId, placement, false, false);
}

bool PlayQueue::replace(const QString &itemId, bool shuffled)
{
    return beginOperation(itemId, Last, true, shuffled);
}

bool PlayQueue::replaceWith(const QStringList &itemIds)
{
    return beginRemote(itemIds, true, Last);
}

bool PlayQueue::addFromRemote(const QStringList &itemIds, Placement placement)
{
    return beginRemote(itemIds, false, placement);
}

bool PlayQueue::beginRemote(const QStringList &itemIds, bool replacing, Placement placement)
{
    if (itemIds.isEmpty() || !std::all_of(itemIds.cbegin(), itemIds.cend(), [](const QString &id) {
            return isValidJellyfinId(id);
        }))
        return false;
    // Each id once, at its first place
    QStringList ids;
    QSet<QString> seen;
    for (const QString &id : itemIds) {
        if (!seen.contains(idKey(id))) {
            seen.insert(idKey(id));
            ids.append(id);
        }
    }
    if (ids.size() == 1)
        return beginOperation(ids.constFirst(), placement, replacing, false, replacing, true);
    if (!mayBegin())
        return false;
    m_op = Operation();
    m_op.active = true;
    m_op.replacing = replacing;
    m_op.placement = placement;
    m_op.remote = true;
    m_op.stage = Stage::Items;
    m_op.cut = ids.size() > MaxEntries;
    m_op.ids = ids.mid(0, MaxEntries);
    if (!requestIdPage()) {
        m_op = Operation();
        return false;
    }
    Q_EMIT busyChanged();
    return true;
}

bool PlayQueue::mayBegin()
{
    if (!m_client || m_op.active || m_restore.active)
        return false;
    // A queue that could not be read back stays untouched until it is: an add would overwrite the file it is still to restore
    if (m_restore.failed) {
        Q_EMIT addFailed();
        return false;
    }
    return true;
}

bool PlayQueue::beginOperation(const QString &itemId, Placement placement, bool replacing,
                               bool shuffled, bool playAlone, bool remote)
{
    if (!isValidJellyfinId(itemId) || !mayBegin())
        return false;
    ItemQuery query;
    query.itemIds = {itemId};
    query.cardImages = true;
    m_op = Operation();
    m_op.active = true;
    m_op.placement = placement;
    m_op.replacing = replacing;
    m_op.shuffled = shuffled;
    m_op.playAlone = playAlone;
    m_op.remote = remote;
    if (!m_client->fetchItems(query, ExpandChannel)) {
        m_op = Operation();
        return false;
    }
    Q_EMIT busyChanged();
    return true;
}

bool PlayQueue::requestIdPage()
{
    ItemQuery query;
    query.itemIds = m_op.ids.mid(m_op.received, PageSize);
    query.cardImages = true;
    return m_client->fetchItems(query, ExpandChannel);
}

void PlayQueue::endOperation()
{
    const bool wasActive = m_op.active;
    if (m_client && wasActive)
        m_client->releaseListingChannel(ExpandChannel);
    m_op = Operation();
    if (wasActive)
        Q_EMIT busyChanged();
}

void PlayQueue::fail()
{
    endOperation();
    Q_EMIT addFailed();
}

void PlayQueue::collect(const QJsonArray &items, bool playableOnly)
{
    for (const QJsonValue &entry : items) {
        if (m_op.collected.size() >= size_t(MaxScan))
            return;
        const ItemSummary summary = parseItemSummary(entry.toObject());
        if (!summary.valid || summary.virtualItem
            || (playableOnly && !isPlayableVideo(summary.type)))
            continue;
        m_op.collected.push_back(summary);
    }
}

void PlayQueue::requestEpisodes(const QString &seriesId, const QString &seasonId)
{
    m_op.stage = Stage::Episodes;
    m_op.received = 0;
    m_op.total = 0;
    m_op.pages = 0;
    m_op.collected.clear();
    if (!m_client->fetchEpisodes(seriesId, seasonId, 0, PageSize, ExpandChannel))
        fail();
}

void PlayQueue::requestChildren()
{
    m_op.stage = Stage::Children;
    ItemQuery query;
    query.parentId = m_op.root.id;
    query.startIndex = m_op.received;
    query.limit = PageSize;
    query.cardImages = true;
    if (!m_client->fetchItems(query, ExpandChannel))
        fail();
}

void PlayQueue::onItems(const QJsonArray &items, int total, int channel)
{
    if (channel == RestoreChannel) {
        onRestoreItems(items);
        return;
    }
    if (channel != ExpandChannel || !m_op.active)
        return;
    if (m_op.stage == Stage::Items) {
        collect(items, true);
        m_op.received += PageSize;
        if (m_op.received < m_op.ids.size()) {
            if (!requestIdPage())
                fail();
            return;
        }
        // The server does not keep the order of the ids it was asked for: the given order is rebuilt
        QHash<QString, ItemSummary> found;
        for (const ItemSummary &item : m_op.collected)
            found.insert(idKey(item.id), item);
        m_op.collected.clear();
        for (const QString &id : std::as_const(m_op.ids)) {
            const auto match = found.constFind(idKey(id));
            if (match != found.constEnd())
                m_op.collected.push_back(*match);
        }
        return finish();
    }
    if (m_op.stage == Stage::Item) {
        const ItemSummary root =
            items.isEmpty() ? ItemSummary() : parseItemSummary(items.first().toObject());
        if (!root.valid)
            return fail();
        m_op.root = root;
        if (isPlayableVideo(root.type)) {
            if (m_op.playAlone && !root.virtualItem) {
                endOperation();
                return Q_EMIT videoAlone(root.id);
            }
            if (!root.virtualItem)
                m_op.collected.push_back(root);
            return finish();
        }
        if (root.type == QLatin1String("Season") && isValidJellyfinId(root.seriesId))
            return requestEpisodes(root.seriesId, root.id);
        if (root.type == QLatin1String("Series")) {
            m_op.stage = Stage::NextUp;
            if (!m_client->fetchNextUp(root.id, QString(), QString(), false, 1, ExpandChannel))
                fail();
            return;
        }
        if (root.type == QLatin1String("BoxSet"))
            return requestChildren();
        // A playlist in its own order, its entries read through the playlist route
        if (root.type == QLatin1String("Playlist"))
            return requestPlaylistEntries();
        endOperation();
        return Q_EMIT nothingToAdd();
    }
    if (m_op.stage != Stage::Children)
        return;
    collect(items, true);
    m_op.received += int(items.size());
    m_op.total = total;
    if (items.isEmpty() || m_op.received >= total)
        return finish();
    if (m_op.received >= MaxScan || ++m_op.pages >= MaxPages
        || m_op.collected.size() >= size_t(MaxScan)) {
        m_op.cut = true;
        return finish();
    }
    requestChildren();
}

void PlayQueue::requestPlaylistEntries()
{
    m_op.stage = Stage::PlaylistEntries;
    if (!m_client->fetchPlaylistEntries(m_op.root.id, m_op.received, PageSize, ExpandChannel))
        fail();
}

void PlayQueue::onPlaylistEntries(const QJsonArray &items, int total, int channel)
{
    if (channel != ExpandChannel || !m_op.active || m_op.stage != Stage::PlaylistEntries)
        return;
    collect(items, true);
    m_op.received += int(items.size());
    m_op.total = total;
    if (items.isEmpty() || m_op.received >= total)
        return finish();
    if (m_op.received >= MaxScan || ++m_op.pages >= MaxPages
        || m_op.collected.size() >= size_t(MaxScan)) {
        m_op.cut = true;
        return finish();
    }
    requestPlaylistEntries();
}

void PlayQueue::onNextUp(const QJsonArray &items, int, int channel)
{
    if (channel != ExpandChannel || !m_op.active || m_op.stage != Stage::NextUp)
        return;
    const ItemSummary next =
        items.isEmpty() ? ItemSummary() : parseItemSummary(items.first().toObject());
    // No Next Up episode: the series plays from its first episode (so does one that lies beyond what an expansion reads)
    m_op.nextUpId = next.valid ? next.id : QString();
    requestEpisodes(m_op.root.id, QString());
}

void PlayQueue::onEpisodes(const QJsonArray &items, int total, int channel)
{
    if (channel != ExpandChannel || !m_op.active || m_op.stage != Stage::Episodes)
        return;
    collect(items, false);
    m_op.received += int(items.size());
    m_op.total = total;
    if (items.isEmpty() || m_op.received >= total)
        return finish();
    if (m_op.received >= MaxScan || ++m_op.pages >= MaxPages
        || m_op.collected.size() >= size_t(MaxScan)) {
        m_op.cut = true;
        return finish();
    }
    if (!m_client->fetchEpisodes(
            m_op.root.type == QLatin1String("Season") ? m_op.root.seriesId : m_op.root.id,
            m_op.root.type == QLatin1String("Season") ? m_op.root.id : QString(), m_op.received,
            PageSize, ExpandChannel))
        fail();
}

void PlayQueue::onListingFailed(const QString &, int, const QString &, int channel)
{
    if (channel == ExpandChannel && m_op.active) {
        fail();
    } else if (channel == RestoreChannel && m_restore.active) {
        // The file stays as it is: a retry reads it again
        m_restore.active = false;
        m_restore.failed = true;
        m_restore.ids.clear();
        m_restore.found.clear();
        Q_EMIT restoringChanged();
        Q_EMIT busyChanged();
    }
}

void PlayQueue::finish()
{
    std::vector<ItemSummary> items = std::move(m_op.collected);
    const Placement placement = m_op.placement;
    const bool season = m_op.root.type == QLatin1String("Season");
    const bool series = m_op.root.type == QLatin1String("Series");
    const QString nextUpId = m_op.nextUpId;
    const bool cut = m_op.cut;
    const bool replacing = m_op.replacing;
    const bool shuffled = m_op.shuffled;
    const bool remote = m_op.remote;
    endOperation();
    // A season starts at its first unplayed episode, a series at its Next Up episode; with nothing to resume at, from the top
    auto start = items.begin();
    if (season) {
        const auto unplayed = std::find_if(items.begin(), items.end(),
                                           [](const ItemSummary &item) { return !item.played; });
        if (unplayed != items.end())
            start = unplayed;
    } else if (series && !nextUpId.isEmpty()) {
        const auto next = std::find_if(items.begin(), items.end(), [&](const ItemSummary &item) {
            return idKey(item.id) == idKey(nextUpId);
        });
        if (next != items.end())
            start = next;
    }
    items.erase(items.begin(), start);
    if (items.empty())
        return Q_EMIT nothingToAdd();
    bool truncated = cut;
    int joined = 0;
    if (replacing) {
        if (shuffled)
            std::shuffle(items.begin(), items.end(), *QRandomGenerator::global());
        replaceAll(std::move(items), &truncated, &joined, remote);
        return Q_EMIT replaced(joined, truncated);
    }
    insert(std::move(items), placement, &truncated, &joined, remote);
    Q_EMIT added(joined, truncated);
}

void PlayQueue::replaceAll(std::vector<ItemSummary> items, bool *truncated, int *joined,
                           bool remote)
{
    // Each id once, at its first place, and the cap on the whole list
    QSet<QString> seen;
    std::vector<ItemSummary> unique;
    for (ItemSummary &item : items) {
        if (seen.contains(idKey(item.id)))
            continue;
        seen.insert(idKey(item.id));
        unique.push_back(std::move(item));
    }
    if (unique.size() > size_t(MaxEntries)) {
        *truncated = true;
        unique.resize(MaxEntries);
    }
    *joined = int(unique.size());
    // One snapshot of the old queue: Undo brings it back whole
    rememberEdit(remote);
    beginResetModel();
    m_items = std::move(unique);
    endResetModel();
    Q_EMIT countChanged();
    Q_EMIT edited();
}

void PlayQueue::insert(std::vector<ItemSummary> items, Placement placement, bool *truncated,
                       int *joined, bool remote)
{
    // Each id once in the batch
    QSet<QString> seen;
    std::vector<ItemSummary> unique;
    for (ItemSummary &item : items) {
        if (!seen.contains(idKey(item.id))) {
            seen.insert(idKey(item.id));
            unique.push_back(std::move(item));
        }
    }
    // The cap applies to what the batch adds: an item queued already moves instead of growing the list
    QSet<QString> queued;
    for (const ItemSummary &item : m_items)
        queued.insert(idKey(item.id));
    const size_t moved =
        size_t(std::count_if(unique.begin(), unique.end(), [&](const ItemSummary &item) {
            return queued.contains(idKey(item.id));
        }));
    const size_t kept = m_items.size() - moved;
    const size_t room = size_t(MaxEntries) > kept ? size_t(MaxEntries) - kept : 0;
    if (unique.size() > room) {
        *truncated = true;
        unique.resize(room);
    }
    *joined = int(unique.size());
    if (unique.empty())
        return;
    rememberEdit(remote);
    // What is queued and survives the cut leaves its row first and joins at the new place
    QSet<QString> incoming;
    for (const ItemSummary &item : unique)
        incoming.insert(idKey(item.id));
    for (int row = count() - 1; row >= 0; --row) {
        if (incoming.contains(idKey(m_items[size_t(row)].id))) {
            beginRemoveRows(QModelIndex(), row, row);
            m_items.erase(m_items.begin() + row);
            endRemoveRows();
        }
    }
    int first = placement == Next ? 0 : count();
    if (placement == Next && !m_currentPlaybackItemId.isEmpty()) {
        const auto playing =
            std::find_if(m_items.begin(), m_items.end(), [this](const ItemSummary &item) {
                return idKey(item.id) == idKey(m_currentPlaybackItemId);
            });
        if (playing != m_items.end())
            first = int(std::distance(m_items.begin(), playing)) + 1;
    }
    beginInsertRows(QModelIndex(), first, first + int(unique.size()) - 1);
    m_items.insert(m_items.begin() + first, unique.begin(), unique.end());
    endInsertRows();
    Q_EMIT countChanged();
    Q_EMIT edited();
}

bool PlayQueue::move(int from, int to)
{
    if (from < 0 || from >= count() || to < 0 || to >= count())
        return false;
    if (from == to)
        return true;
    // beginMoveRows names the row the item goes in front of
    if (!beginMoveRows(QModelIndex(), from, from, QModelIndex(), to > from ? to + 1 : to))
        return false;
    rememberEdit();
    ItemSummary item = std::move(m_items[size_t(from)]);
    m_items.erase(m_items.begin() + from);
    m_items.insert(m_items.begin() + to, std::move(item));
    endMoveRows();
    Q_EMIT edited();
    return true;
}

bool PlayQueue::remove(int row)
{
    if (row < 0 || row >= count())
        return false;
    rememberEdit();
    beginRemoveRows(QModelIndex(), row, row);
    m_items.erase(m_items.begin() + row);
    endRemoveRows();
    Q_EMIT countChanged();
    Q_EMIT edited();
    return true;
}

void PlayQueue::clear()
{
    // An expansion still running was meant for the list that is gone: it ends without a word
    endOperation();
    if (m_items.empty())
        return;
    rememberEdit();
    beginResetModel();
    m_items.clear();
    endResetModel();
    Q_EMIT countChanged();
    Q_EMIT edited();
}

void PlayQueue::rememberEdit(bool remote)
{
    // Edits another session asked for keep the snapshot of the queue the user last shaped, so one Undo brings it back after any number of them
    if (remote && m_undoBeforeRemote && m_canUndo)
        return;
    // Keep at most one snapshot (500 bounded ItemSummary objects), not a history
    // of server responses or a second copy of the persisted queue.
    m_undoItems = m_items;
    m_undoBeforeRemote = remote;
    if (!m_canUndo) {
        m_canUndo = true;
        Q_EMIT canUndoChanged();
    }
}

void PlayQueue::forgetUndo()
{
    m_undoItems.clear();
    m_undoBeforeRemote = false;
    if (m_canUndo) {
        m_canUndo = false;
        Q_EMIT canUndoChanged();
    }
}

bool PlayQueue::undo()
{
    // A pending expansion could insert stale rows after the undo; a failed
    // restore must not be overwritten with an edited local queue.
    if (!m_canUndo || busy() || m_restore.failed)
        return false;
    const int oldCount = count();
    std::vector<ItemSummary> previous = std::move(m_undoItems);
    forgetUndo();
    beginResetModel();
    m_items = std::move(previous);
    endResetModel();
    if (count() != oldCount)
        Q_EMIT countChanged();
    Q_EMIT edited(); // Persist the reverted order, including an empty queue.
    return true;
}

void PlayQueue::dropMemory()
{
    // Even an empty queue may have a previous non-empty snapshot (after Clear).
    forgetUndo();
    if (m_items.empty())
        return;
    beginResetModel();
    m_items.clear();
    endResetModel();
    Q_EMIT countChanged();
}

void PlayQueue::setStoragePath(const QString &path)
{
    m_path = path;
}

void PlayQueue::setSaveDelay(std::chrono::milliseconds delay)
{
    m_saveTimer.setInterval(delay);
}

void PlayQueue::scheduleSave()
{
    // Without a file or an owner there is nothing to keep
    if (ownerFile(m_serverId, m_userId).isEmpty())
        return;
    m_dirty = true;
    m_saveTimer.start();
}

void PlayQueue::flush()
{
    m_saveTimer.stop();
    if (!m_dirty)
        return;
    m_dirty = false;
    const QString file = ownerFile(m_serverId, m_userId);
    if (file.isEmpty())
        return;
    if (m_items.empty()) {
        // Removing what is not there is a success
        QFile::remove(file);
        m_saveFailing = false;
        return;
    }
    if (writePrivateFile(file, queueFileBytes(m_serverId, m_userId, itemIds()))) {
        m_saveFailing = false;
    } else if (!m_saveFailing) {
        m_saveFailing = true;
        Q_EMIT saveFailed();
    }
}

QString PlayQueue::queuesDirectory() const
{
    return m_path.isEmpty() ? QString()
                            : QFileInfo(m_path).absolutePath() + QStringLiteral("/queues");
}

QString PlayQueue::ownerFile(const QString &serverId, const QString &userId) const
{
    // Valid ids hold no path separator and no dot: the name cannot leave the folder
    if (m_path.isEmpty() || !isValidJellyfinId(serverId) || !isValidJellyfinId(userId))
        return {};
    return queuesDirectory() + QLatin1Char('/') + serverId.toLower() + QLatin1Char('_')
           + userId.toLower() + QStringLiteral(".json");
}

PlayQueue::Stored PlayQueue::readStored(const QString &path, StoredQueue *stored)
{
    if (path.isEmpty() || !QFileInfo::exists(path))
        return Stored::Missing;
    const auto reject = [&path] {
        QFile::remove(path);
        return Stored::Rejected;
    };
    // A link, a pipe or a device is no queue file: it is never opened (a pipe would block, /dev/zero would never end)
    const QFileInfo info(path);
    if (info.isSymLink() || !info.isFile() || info.size() > MaxFileBytes)
        return reject();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return reject();
    // The file may have grown since it was measured: one byte past the limit is enough to tell
    const QByteArray bytes = file.read(MaxFileBytes + 1);
    file.close();
    if (bytes.size() > MaxFileBytes)
        return reject();
    const QJsonObject root = QJsonDocument::fromJson(bytes).object();
    const QJsonValue version = root.value(QStringLiteral("version"));
    if (version.isDouble() && version.toInt() > FileVersion) {
        // A later version wrote it: keep it aside rather than losing it
        QFile::remove(path + QStringLiteral(".broken"));
        QFile::rename(path, path + QStringLiteral(".broken"));
        return Stored::Rejected;
    }
    const QString serverId = root.value(QStringLiteral("serverId")).toString();
    const QString userId = root.value(QStringLiteral("userId")).toString();
    const QJsonValue items = root.value(QStringLiteral("items"));
    if (!version.isDouble() || version.toInt() != FileVersion || !isValidJellyfinId(serverId)
        || !isValidJellyfinId(userId) || !items.isArray() || items.toArray().size() > MaxEntries)
        return reject();
    QStringList ids;
    QSet<QString> seen;
    for (const QJsonValue &entry : items.toArray()) {
        const QString id = entry.toString();
        if (!isValidJellyfinId(id) || seen.contains(idKey(id)))
            return reject();
        seen.insert(idKey(id));
        ids.append(id);
    }
    stored->serverId = serverId;
    stored->userId = userId;
    stored->ids = ids;
    return Stored::Ok;
}

void PlayQueue::endRestore()
{
    const bool changed = m_restore.active || m_restore.failed;
    const bool wasActive = m_restore.active;
    if (m_client && m_restore.active)
        m_client->releaseListingChannel(RestoreChannel);
    m_restore = Restore();
    if (changed)
        Q_EMIT restoringChanged();
    if (wasActive)
        Q_EMIT busyChanged();
}

void PlayQueue::restoreFor(const QString &serverId, const QString &userId)
{
    m_currentPlaybackItemId.clear();
    // What was edited under the previous owner goes to its file before another is read
    flush();
    endRestore();
    endOperation();
    // The rows in memory belong to the previous owner
    dropMemory();
    // An owner that cannot name a file keeps nothing
    const bool named = !ownerFile(serverId, userId).isEmpty();
    m_serverId = named ? serverId : QString();
    m_userId = named ? userId : QString();
    if (!m_client || !named)
        return;
    migrateLegacy(serverId, userId);
    const QString file = ownerFile(serverId, userId);
    StoredQueue stored;
    if (readStored(file, &stored) != Stored::Ok)
        return;
    // A file that is not what its name says is no queue of this owner
    if (!sameId(stored.serverId, serverId) || !sameId(stored.userId, userId)) {
        QFile::remove(file);
        return;
    }
    if (stored.ids.isEmpty())
        return;
    m_restore.active = true;
    m_restore.ids = stored.ids;
    Q_EMIT restoringChanged();
    Q_EMIT busyChanged();
    requestRestoreBatch();
}

void PlayQueue::migrateLegacy(const QString &serverId, const QString &userId)
{
    // The one-file queue of earlier versions belongs to the owner it names; another owner leaves it alone
    const QString file = ownerFile(serverId, userId);
    StoredQueue legacy;
    if (QFileInfo(file).isSymLink() || QFileInfo::exists(file)
        || readStored(m_path, &legacy) != Stored::Ok || !sameId(legacy.serverId, serverId)
        || !sameId(legacy.userId, userId))
        return;
    // The old file goes only once the new one is written
    if (writePrivateFile(file, queueFileBytes(serverId, userId, legacy.ids)))
        QFile::remove(m_path);
}

void PlayQueue::retryRestore()
{
    if (!m_restore.failed || m_serverId.isEmpty())
        return;
    restoreFor(m_serverId, m_userId);
}

void PlayQueue::requestRestoreBatch()
{
    ItemQuery query;
    query.itemIds = m_restore.ids.mid(m_restore.next, PageSize);
    query.cardImages = true;
    if (!m_client->fetchItems(query, RestoreChannel))
        onListingFailed(QString(), 0, QString(), RestoreChannel);
}

void PlayQueue::onRestoreItems(const QJsonArray &items)
{
    if (!m_restore.active)
        return;
    for (const QJsonValue &entry : items) {
        const ItemSummary summary = parseItemSummary(entry.toObject());
        if (summary.valid && isPlayableVideo(summary.type) && !summary.virtualItem)
            m_restore.found.insert(idKey(summary.id), summary);
    }
    m_restore.next += PageSize;
    if (m_restore.next < m_restore.ids.size())
        return requestRestoreBatch();
    finishRestore();
}

void PlayQueue::finishRestore()
{
    // The server does not keep the order of the ids it was asked for: the stored order is rebuilt
    std::vector<ItemSummary> rows;
    for (const QString &id : std::as_const(m_restore.ids)) {
        const auto found = m_restore.found.constFind(idKey(id));
        if (found != m_restore.found.constEnd())
            rows.push_back(*found);
    }
    const bool dropped = rows.size() < size_t(m_restore.ids.size());
    endRestore();
    if (!rows.empty()) {
        beginResetModel();
        m_items = std::move(rows);
        endResetModel();
        Q_EMIT countChanged();
    }
    // What the server no longer has leaves the file too
    if (dropped)
        scheduleSave();
    // An empty answer for a stored queue leaves an empty queue: the file goes with the next save
    if (m_items.empty() && dropped)
        scheduleSave();
}

void PlayQueue::forgetStored(const QString &serverId, const QString &userId)
{
    if (!m_path.isEmpty() && isValidJellyfinId(serverId)
        && (userId.isEmpty() || isValidJellyfinId(userId))) {
        if (userId.isEmpty()) {
            const QDir folder(queuesDirectory());
            const QString prefix = serverId.toLower() + QLatin1Char('_');
            // A file set aside as `.broken` is the user's queue as well
            for (const QString &name :
                 folder.entryList(QDir::Files | QDir::System | QDir::Hidden)) {
                if (name.startsWith(prefix)
                    && (name.endsWith(QLatin1String(".json"))
                        || name.endsWith(QLatin1String(".json.broken"))))
                    QFile::remove(folder.filePath(name));
            }
        } else {
            const QString file = ownerFile(serverId, userId);
            QFile::remove(file);
            QFile::remove(file + QStringLiteral(".broken"));
        }
        // The queue of earlier versions, when it is this owner's
        StoredQueue legacy;
        if (readStored(m_path, &legacy) == Stored::Ok && sameId(legacy.serverId, serverId)
            && (userId.isEmpty() || sameId(legacy.userId, userId)))
            QFile::remove(m_path);
    }
    // Pending edits and rows belong to the owner in memory: another server's removal leaves them alone
    if (sameId(m_serverId, serverId) && (userId.isEmpty() || sameId(m_userId, userId))) {
        m_saveTimer.stop();
        m_dirty = false;
        endOperation();
        endRestore();
        dropMemory();
        m_serverId.clear();
        m_userId.clear();
    }
}

void PlayQueue::forgetStoredUser(const QString &serverId, const QString &userId)
{
    // An empty user would stand for the whole server
    if (!userId.isEmpty())
        forgetStored(serverId, userId);
}
} // namespace Kinefin
