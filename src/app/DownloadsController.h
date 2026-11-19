// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/JellyfinClient.h"
#include "downloads/DownloadManager.h"
#include "downloads/DownloadRules.h"
#include "downloads/WatchedOutbox.h"
#include "integration/NetworkMeter.h"
#include "models/DownloadsModel.h"

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QThread>
#include <QUrl>
#include <QVariantMap>

#include <memory>
#include <optional>

namespace Kinefin {
class ImageNetworkAccess;
class ImageOrigin;

// The download queue of the signed-in session: owns the engine and the model the Downloads page shows, asks the server for the item fields a download needs, and builds the resolver the engine calls at start time (the URL and the Authorization header, never stored). A manual download runs on any network; the engine's automatic ones follow the meter and the "only on unmetered networks" setting, failing closed while the meter is unknown
class DownloadsController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(Kinefin::DownloadsModel *model READ model CONSTANT)
    // `Policy.EnableContentDownloading` of the signed-in user; false while unknown, so every action hides
    Q_PROPERTY(bool canDownload READ canDownload NOTIFY canDownloadChanged)
    Q_PROPERTY(bool online READ online NOTIFY onlineChanged)
    // The current network is metered, or its state is unknown: a manual download asks first
    Q_PROPERTY(bool metered READ metered NOTIFY meteredChanged)
    // Bumped on every record change, so a QML binding that reads a record's state follows the queue
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)
    // The index could not be written: the queue lives for this session only
    Q_PROPERTY(bool storageFailed READ storageFailed NOTIFY storageFailedChanged)
    // Watched-state events of the current owner waiting for the server
    Q_PROPERTY(int pendingSyncCount READ pendingSyncCount NOTIFY outboxChanged)
    // Watched-state events of the current owner the server contradicts; each has Keep Server / Mark Played actions
    Q_PROPERTY(QVariantList outboxConflicts READ outboxConflicts NOTIFY outboxChanged)
    // The owner of what the page lists: the signed-in user, or the saved user shown in Offline; both empty when there is none
    Q_PROPERTY(QString ownerServerId READ ownerServerId NOTIFY ownerChanged)
    Q_PROPERTY(QString ownerUserId READ ownerUserId NOTIFY ownerChanged)
    // Bumped when a series rule changes, so the series page follows it
    Q_PROPERTY(int rulesRevision READ rulesRevision NOTIFY rulesRevisionChanged)

  public:
    DownloadsController(JellyfinClient *client, const QString &profileId,
                        QObject *parent = nullptr);
    ~DownloadsController() override;

    DownloadsModel *model() { return &m_model; }
    DownloadStore *store() { return m_manager.store(); }
    const DownloadStore *store() const { return m_manager.store(); }
    bool canDownload() const;
    bool online() const { return m_online; }
    bool metered() const { return m_meter.metered(); }
    int revision() const { return m_revision; }
    bool storageFailed() const { return m_storageFailed; }
    int pendingSyncCount() const;
    QVariantList outboxConflicts() const;
    QString ownerServerId() const;
    QString ownerUserId() const;
    int rulesRevision() const { return m_rulesRevision; }

    // The session can serve downloads; false stops the active transfers and holds the queue
    void setOnline(bool online);
    void setParallelDownloads(int count);
    // When false, automatic downloads may start on a metered network too; manual ones always may
    void setUnmeteredOnly(bool unmeteredOnly);
    void setIgnoreCertificateErrors(bool ignore);
    // Re-reads the policy after a login or a profile refresh
    void refreshCanDownload();
    // The saved user whose downloads Offline shows (no session then); empty ids go back to the signed-in user
    void setOfflineOwner(const QString &serverId, const QString &userId);
    // The origin the poster fetch is limited to, shared with the QML images; without one no poster is fetched
    void setImageOrigin(std::shared_ptr<ImageOrigin> origin);
    // Whether a completed record still has its media file
    bool hasLocalFile(const DownloadRecord &record) const;

    // Fetches the item's fields and enqueues the original file; false when the policy denies it, the id is invalid or a fetch is already in flight. A later failure arrives as downloadFailed
    Q_INVOKABLE bool downloadItem(const QString &itemId);
    // The record's state for the current owner, empty when there is none
    Q_INVOKABLE QString downloadState(const QString &itemId) const;
    Q_INVOKABLE bool pauseDownload(const QString &serverId, const QString &userId,
                                   const QString &itemId);
    Q_INVOKABLE bool retryDownload(const QString &serverId, const QString &userId,
                                   const QString &itemId);
    Q_INVOKABLE bool removeDownload(const QString &serverId, const QString &userId,
                                    const QString &itemId);
    Q_INVOKABLE bool removeCurrentDownload(const QString &itemId);
    // A byte count in the user's locale and units ("1.5 GB"); empty for an unknown size
    Q_INVOKABLE QString formattedSize(qint64 bytes) const;
    // The local file of a completed download and what the player shows for it; empty when it is not playable
    Q_INVOKABLE QVariantMap playableDownload(const QString &serverId, const QString &userId,
                                             const QString &itemId) const;
    // The token-free poster URL of a record; empty without a tag or a session
    Q_INVOKABLE QUrl posterUrl(const QString &itemId, const QString &tag) const;
    // The cached poster of a record when there is one, else the server image URL
    Q_INVOKABLE QUrl posterSource(const QString &serverId, const QString &userId,
                                  const QString &itemId, const QString &tag) const;
    // Stores where a local playback of that owner's record stopped; a watched item keeps position 0 and queues one watched-state event
    Q_INVOKABLE void updateLocalProgress(const QString &serverId, const QString &userId,
                                         const QString &itemId, qint64 ticks, bool played);
    // Sends the queued watched-state events, one at a time; called at sign-in, on reconnect and when the page opens online
    Q_INVOKABLE void reconcileOutbox();
    // Resolves a conflict: keepServer drops the event, otherwise it marks the item played on the server
    Q_INVOKABLE bool resolveOutboxConflict(const QString &serverId, const QString &userId,
                                           const QString &itemId, bool keepServer);
    // Copies a completed download below `folder` (a host folder chosen through the portal) and marks it exported; the sandbox copy stays
    Q_INVOKABLE void exportDownload(const QString &serverId, const QString &userId,
                                    const QString &itemId, const QUrl &folder);
    void setKodiSidecars(bool on) { m_kodiSidecars = on; }
    // Automatic rules of a series: nextN (count), season (seasonNumber) or newEpisodes; the owner is the signed-in user
    Q_INVOKABLE bool setSeriesRule(const QString &seriesId, const QString &seriesName,
                                   const QString &kind, int seasonNumber = -1, int count = 3);
    Q_INVOKABLE QVariantList seriesRules() const;
    Q_INVOKABLE bool removeSeriesRule(const QString &seriesId);
    Q_INVOKABLE QVariantMap seriesRule(const QString &seriesId) const;
    // Runs the enabled rules now (startup, sign-in, reconnect, LibraryChanged, every 6 h)
    Q_INVOKABLE void scanRules();
    // A LibraryChanged hint wakes the scanner, debounced; the event is never treated as an item
    void scheduleRuleScan();

  Q_SIGNALS:
    void canDownloadChanged();
    void onlineChanged();
    void meteredChanged();
    void revisionChanged();
    void storageFailedChanged();
    void outboxChanged();
    void rulesRevisionChanged();
    void exportFinished(const QString &itemId, bool ok);
    // Another export still runs: nothing was started
    void exportBusy(const QString &itemId);
    void ownerChanged();
    // A manual download could not be enqueued: "forbidden", "notfound", "unsupported", "network", "queue" or "failed"; the downloads of a rule stay silent
    void downloadFailed(const QString &itemId, const QString &reason);
    void downloadEnqueued(const QString &itemId);

  private:
    void onItemsReady(const QJsonArray &items, int totalRecordCount, int channel);
    void onOutboxItemsReady(const QJsonArray &items, int channel);
    void onEpisodesReady(const QJsonArray &items, int totalRecordCount, int channel);
    bool downloadItemForRule(const QString &itemId, const QString &ruleId,
                             const QDateTime &created = {});
    // A rule download that failed after the scan moved the cursor past it: the cursor goes back before the episode, so the next scan offers it again
    void rewindRuleCursor(const QString &seriesId, const QDateTime &created);
    void reconcileNext();
    void refreshOwner();
    bool ownedByCurrent(const QString &serverId, const QString &userId) const;
    void writeInfo(const DownloadRecord &record);
    void onListingFailed(const QString &family, int httpStatus, const QString &reason, int channel);
    std::optional<DownloadRecord> recordFor(const QJsonObject &item, const QString &ruleId) const;

    JellyfinClient *m_client = nullptr;
    DownloadManager m_manager;
    DownloadsModel m_model{&m_manager};
    WatchedOutbox m_outbox;
    DownloadRules m_rules;
    QTimer m_ruleTimer;
    QTimer m_ruleDebounce;
    NetworkMeter m_meter;
    // The item each pending one-shot item fetch was asked for
    QHash<int, QString> m_fetches;
    QHash<int, QString> m_fetchRules;
    // The creation date of the episode each rule fetch is for
    QHash<int, QDateTime> m_fetchRuleDates;
    // Outbox reconciliation: the channel of the item read, by event key, and the item whose played write is in flight
    QHash<int, QString> m_outboxFetches;
    QHash<int, QString> m_outboxVerifies;
    std::unique_ptr<ImageNetworkAccess> m_images;
    QString m_offlineServerId;
    QString m_offlineUserId;
    QString m_ownerKey;
    QThread *m_exportThread = nullptr;
    QSet<QString> m_outboxWrites;
    // The rule of each pending episodes listing
    QHash<int, DownloadRule> m_ruleFetches;
    // Episodes accumulated across the pages of one rule scan, by rule key
    QHash<QString, QList<QJsonObject>> m_ruleItems;
    int m_rulesRevision = 0;
    QSet<QString> m_fetching;
    int m_nextChannel = 2'000'000;
    int m_revision = 0;
    bool m_online = false;
    bool m_canDownload = false;
    bool m_unmeteredOnly = true;
    bool m_ignoreCertificateErrors = false;
    bool m_storageFailed = false;
    bool m_kodiSidecars = false;
};
} // namespace Kinefin
