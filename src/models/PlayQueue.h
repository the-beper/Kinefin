// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/JellyfinClient.h"
#include "models/ItemSummary.h"

#include <QAbstractListModel>
#include <QJsonArray>
#include <QPointer>
#include <QTimer>

#include <chrono>

#include <vector>

namespace Kinefin {
// The picture of a list row: the item's own, else (an episode without one) its parent's backdrop; empty when the server named neither
QUrl listImageUrl(const JellyfinClient *client, const ItemSummary &item, int width);

// The list of what plays next: video items in order, each at most once, at most `MaxEntries`. `add()` takes only an item id and asks the server what it is: a movie, episode or video joins as it is, a season from its first unplayed episode (all of it when every episode is played), a series from its Next Up episode on, a collection with its playable children, a playlist with its playable entries in its order; anything else is refused. `replace()` makes the whole list what an item stands for. One expansion runs at a time on `ExpandChannel` and the rows go in together at its end, so a failure adds nothing. Adding an id that is queued already moves it to the new place. The list empties itself when the client loses its credentials. Names are server text: render them as plain text

class PlayQueue final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool restoring READ restoring NOTIFY restoringChanged)
    Q_PROPERTY(bool restoreFailed READ restoreFailed NOTIFY restoringChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY canUndoChanged)

  public:
    enum Placement { Last, Next };
    Q_ENUM(Placement)
    enum Role {
        ItemIdRole = Qt::UserRole + 1,
        NameRole,
        SeriesNameRole,
        SeasonNumberRole,
        EpisodeNumberRole,
        YearRole,
        RuntimeMinutesRole,
        TypeRole,
        ImageUrlRole,
        PlayedRole
    };

    static constexpr int MaxEntries = 500;
    static constexpr int PageSize = 100;
    // How many entries an expansion reads before it stops: a series starts at its Next Up episode, which may lie beyond the cap of the list
    static constexpr int MaxScan = 2000;
    // A server that lies about its total, or fills pages with entries that are skipped, ends the expansion after this many pages
    static constexpr int MaxPages = 30;
    static constexpr int ExpandChannel = 2048;
    static constexpr int RestoreChannel = 2049;
    // The largest queue file read back, and the format version it carries
    static constexpr qint64 MaxFileBytes = 64 * 1024;
    static constexpr int FileVersion = 1;
    static constexpr int ImageWidth = 240;

    explicit PlayQueue(JellyfinClient *client, QObject *parent = nullptr);
    ~PlayQueue() override;

    int count() const { return int(m_items.size()); }
    bool busy() const { return m_op.active || m_restore.active; }
    bool restoring() const { return m_restore.active; }
    bool restoreFailed() const { return m_restore.failed; }
    bool canUndo() const { return m_canUndo; }

    // Queues the item (and what it stands for) at the end or at the top; false, with nothing sent, when the id is not valid, an expansion is running or the client is not signed in. The outcome comes as `added` or `addFailed`
    Q_INVOKABLE bool add(const QString &itemId, Kinefin::PlayQueue::Placement placement);
    // The whole queue becomes what the item stands for (a playlist in its order), as one step Undo takes back; `shuffled` puts the rows in a random order once (the player's Shuffle stays as it is). Same refusals as add; the outcome comes as `replaced`, `addFailed` or `nothingToAdd`
    Q_INVOKABLE bool replace(const QString &itemId, bool shuffled);
    Q_INVOKABLE bool move(int from, int to);
    Q_INVOKABLE bool remove(int row);
    Q_INVOKABLE void clear();
    // Revert the most recent edit to this owner's queue, without refetching rows.
    // A restore, logout or owner change discards the snapshot; there is no redo.
    Q_INVOKABLE bool undo();

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Entries in order
    QStringList itemIds() const;
    // The row of an item, -1 when it is not queued
    Q_INVOKABLE int indexOf(const QString &itemId) const;
    // The picture of a row: the item's own, else (an episode without one) its parent's backdrop; empty when the server named neither
    QUrl imageUrlFor(const ItemSummary &item) const;
    // When a queue item is playing, Next inserts immediately after that item.
    // An empty id restores the traditional insert-at-front behavior.
    void setCurrentPlaybackItemId(const QString &id);

    // Where the queues are kept: ids only, one file per server and user in the `queues` folder next to `path`, which itself is the one-file queue of earlier versions (read and moved to its owner's file at that owner's first `restoreFor`). Empty, the default, keeps nothing. Writes are debounced and a pending one is flushed when the queue is destroyed or loses its credentials
    void setStoragePath(const QString &path);
    void setSaveDelay(std::chrono::milliseconds delay);
    // A user signed in: the stored queue of this server and user is read back (the ids asked from the server in batches, the stored order kept, what the server no longer has dropped). The files of other users are never touched; an owner whose ids are not valid ids keeps nothing
    void restoreFor(const QString &serverId, const QString &userId);
    // The user signed out or the server was removed: the stored queue of that server (of that user when one is named) goes, and the one in memory with it
    void forgetStored(const QString &serverId, const QString &userId = QString());
    // One user's queue goes (stored and in memory when it is the current one); the other users of the server keep theirs
    void forgetStoredUser(const QString &serverId, const QString &userId);
    Q_INVOKABLE void retryRestore();

  Q_SIGNALS:
    void countChanged();
    void busyChanged();
    // `count` rows joined; `truncated` when the cap cut the expansion short
    void added(int count, bool truncated);
    // The queue was replaced by `count` rows; `truncated` when the cap cut them
    void replaced(int count, bool truncated);
    void addFailed();
    // The item is not something the queue takes, or has nothing to play
    void nothingToAdd();
    // The rows changed by a user edit or an add (not by a session loss or a restore)
    void edited();
    void canUndoChanged();
    void restoringChanged();
    // A write failed (once per streak of failures)
    void saveFailed();

  private:
    enum class Stage { Item, NextUp, Episodes, Children, PlaylistEntries };
    struct Operation {
        bool active = false;
        Placement placement = Last;
        // replace() rather than add(), and its random order
        bool replacing = false;
        bool shuffled = false;
        Stage stage = Stage::Item;
        ItemSummary root;
        QString nextUpId;
        std::vector<ItemSummary> collected;
        int total = 0;
        int received = 0;
        int pages = 0;
        // The scan stopped before the end of what the server holds
        bool cut = false;
    };

    struct Restore {
        bool active = false;
        bool failed = false;
        QStringList ids;
        qsizetype next = 0;
        QHash<QString, ItemSummary> found;
    };
    enum class Stored { Missing, Ok, Rejected };
    struct StoredQueue {
        QString serverId;
        QString userId;
        QStringList ids;
    };

    void onItems(const QJsonArray &items, int total, int channel);
    void onNextUp(const QJsonArray &items, int total, int channel);
    void onEpisodes(const QJsonArray &items, int total, int channel);
    void onPlaylistEntries(const QJsonArray &items, int total, int channel);
    bool beginOperation(const QString &itemId, Placement placement, bool replacing, bool shuffled);
    void requestPlaylistEntries();
    void onListingFailed(const QString &family, int status, const QString &reason, int channel);
    void requestEpisodes(const QString &seriesId, const QString &seasonId);
    void requestChildren();
    void collect(const QJsonArray &items, bool playableOnly);
    void finish();
    void fail();
    void endOperation();
    void insert(std::vector<ItemSummary> items, Placement placement, bool *truncated, int *joined);
    void replaceAll(std::vector<ItemSummary> items, bool *truncated, int *joined);

    Stored readStored(const QString &path, StoredQueue *stored);
    QString queuesDirectory() const;
    // The file of an owner; empty without a storage path or for ids that are not valid
    QString ownerFile(const QString &serverId, const QString &userId) const;
    void migrateLegacy(const QString &serverId, const QString &userId);
    void requestRestoreBatch();
    void onRestoreItems(const QJsonArray &items);
    void finishRestore();
    void endRestore();
    void dropMemory();
    void rememberEdit();
    void forgetUndo();
    void scheduleSave();
    void flush();

    QPointer<JellyfinClient> m_client;
    std::vector<ItemSummary> m_items;
    std::vector<ItemSummary> m_undoItems;
    bool m_canUndo = false;
    Operation m_op;
    Restore m_restore;
    QString m_path;
    QString m_currentPlaybackItemId;
    QString m_serverId;
    QString m_userId;
    QTimer m_saveTimer;
    bool m_dirty = false;
    bool m_saveFailing = false;
};
} // namespace Kinefin
