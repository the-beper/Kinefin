// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QUrl>

#include <functional>
#include <optional>

#include "downloads/DownloadStore.h"

namespace Kinefin {
class DownloadTransfer;

// Where a queued record can be fetched from right now: the URL and the Authorization header are asked of the session at start time, never stored on disk. Empty means the session cannot serve the record yet (signed out, server offline) and the record waits
struct DownloadSource {
    QUrl url;
    QByteArray authorization;
    bool ignoreCertificateErrors = false;
};

// The download queue of one profile: at most `MaxParallel` transfers at once, each in its own thread, and a persisted record per item. The manager never stores a URL or a token; `sourceResolver` builds both from the record when a transfer starts. A manual enqueue (empty `ruleId`) runs even on a metered network; an automatic one waits, failing closed while the meter is unknown. Records survive a restart: a `fetching` record becomes `queued`, a `paused` one stays paused, and a completed file is only played through `localPath()`
class DownloadManager final : public QObject {
    Q_OBJECT

  public:
    static constexpr int MaxParallel = 4;
    static constexpr int DefaultParallel = 2;

    using SourceResolver = std::function<std::optional<DownloadSource>(const DownloadRecord &)>;

    explicit DownloadManager(QObject *parent = nullptr);
    ~DownloadManager() override;

    // `<data>/downloads/index.json` of the profile; empty keeps the queue in memory. Changing it stops every transfer first
    void setStoragePath(const QString &indexPath);
    QString storagePath() const;
    DownloadStore *store();
    const DownloadStore *store() const;

    void setParallelDownloads(int count);
    int parallelDownloads() const;

    // Whether the session can serve downloads at all; false stops the active transfers (their partials are kept) and holds the queue
    void setOnline(bool online);
    bool online() const;
    // Whether the current network is metered; it starts metered, so automatic downloads wait until the caller knows the answer and automatic transfers already running stop
    void setMetered(bool metered);
    bool metered() const;

    void setSourceResolver(SourceResolver resolver);

    // Inserts or replaces the record and schedules it; false when the store refused it
    // `batch` true leaves the write of the index to the store's own timer and does not start a transfer yet: for a batch, followed by one flush of the store and one wake()
    bool enqueue(const DownloadRecord &record, bool batch = false);
    // Queues a record again after a failure, or resumes a paused one
    bool retry(const QString &serverId, const QString &userId, const QString &itemId);
    // Runs the queue now: for a gate that opened without setOnline or setMetered changing (the download policy became known)
    void wake();
    // Stores the fields a caller owns (local position, played, exported state, info fields) of an existing record and emits recordChanged; false for an unknown key. While a transfer runs, its state, size, byte count, container and ETag are kept as they are
    bool updateRecord(const DownloadRecord &record);
    // Sets a record's state (export) and tells the model; false for an unknown record
    bool setState(const QString &serverId, const QString &userId, const QString &itemId,
                  const QString &state);
    // Stops the transfer and keeps the partial file for a later resume; wanted is cleared
    bool pause(const QString &serverId, const QString &userId, const QString &itemId);
    // Drops the item: its whole folder (media, partial, sidecar, poster, info, subtitles) goes, then the record
    bool remove(const QString &serverId, const QString &userId, const QString &itemId);

    // The validated path of a completed file; empty for anything else
    QString localPath(const DownloadRecord &record) const;
    QString localPath(const QString &serverId, const QString &userId, const QString &itemId) const;

    // Bounded automatic retries for rule records: delays in milliseconds, one attempt per entry (empty disables them)
    void setRetryDelays(const QList<int> &milliseconds);
    QList<int> retryDelays() const;

  Q_SIGNALS:
    // Any change, including progress
    void recordsChanged();
    void recordChanged(const Kinefin::DownloadRecord &record);
    // A file became playable, or was dropped
    void downloadCompleted(const Kinefin::DownloadRecord &record);
    void downloadRemoved(const QString &serverId, const QString &userId, const QString &itemId);
    // The index could not be written; the queue keeps running in memory
    void storageError();

  private:
    enum class Stop {
        None,
        Pause,
        Hold,
        Remove,
    };

    void pump();
    void start(const DownloadRecord &record, const DownloadSource &source);
    void release(const QString &key);
    void finishTransfer(const QString &key, bool completed, qint64 size, const QString &etag,
                        int failure, int httpStatus);
    // False when the record was already in that state; `writeNow` false batches the write for the caller
    bool setRecordState(const QString &key, const QString &state, const QString &error = {},
                        int httpStatus = 0, bool writeNow = true);
    bool startable(const DownloadRecord &record) const;
    void dropFiles(const DownloadRecord &record);
    void scheduleRetry(const QString &key, int httpStatus);
    void stopActive(const QString &key, Stop reason);
    void stopAll(Stop reason);
    static QString failureToken(int failure);
    QString targetPath(const DownloadRecord &record) const;
    QString partialPath(const DownloadRecord &record) const;
    QString sidecarPath(const DownloadRecord &record) const;
    QString resolveKey(const DownloadRecord &record) const;

    DownloadStore m_store;
    SourceResolver m_resolver;
    QHash<QString, DownloadTransfer *> m_active;
    QHash<QString, Stop> m_stopping;
    // The queue generation of a record a running transfer was removed with, so a late cancel never drops a record enqueued after it
    QHash<QString, quint64> m_removing;
    QHash<QString, quint64> m_generation;
    QHash<QString, int> m_retryAttempt;
    QSet<QString> m_retryPending;
    QList<int> m_retryDelays{30'000, 120'000, 600'000};
    int m_parallel = DefaultParallel;
    bool m_online = true;
    bool m_metered = true;
};
} // namespace Kinefin
