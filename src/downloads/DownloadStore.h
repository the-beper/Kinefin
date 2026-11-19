// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QList>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QTimer>

#include <optional>

namespace Kinefin {
// One downloaded item, as the queue keeps it: server ids and ids only for paths, server text (name, series name) capped and never used as a path. `state` is the user-visible state, `error` a fixed reason token, and `bytesWritten` the sidecar's last count; a `fetching` record found after a restart becomes `queued` again
struct DownloadRecord {
    static constexpr int MaxName = 512;
    static constexpr int MaxEtag = 256;
    static constexpr int MaxContainer = 8;
    static constexpr int MaxTag = 128;
    // The largest index numbers and runtime a record accepts; a larger value from the server is clamped before the store sees it
    static constexpr int MaxIndexNumber = 10'000;
    static constexpr qint64 MaxRuntimeTicks = qint64(1) << 40;

    QString serverId;
    QString userId;
    QString itemId;
    // Remote text, rendered as plain text
    QString name;
    QString seriesName;
    QString seriesId;
    QString seasonId;
    // Movie, Episode or Video
    QString type;
    // The server's container, letters and digits only; `bin` when it sent none
    QString container;
    // The item's own ETag (MediaSources[].ETag) the download was started with, empty when the server sent none; never an HTTP header value
    QString etag;
    // The primary image tag the row uses for its poster, empty when the server sent none
    QString primaryTag;
    // queued, fetching, paused, waiting, error, completed or exported
    QString state = QStringLiteral("queued");
    // network, http, protocol, size, changed, io, nospace or cancelled; empty unless state is error
    QString error;
    int httpStatus = 0;
    int seasonNumber = -1;
    int episodeNumber = -1;
    int year = 0;
    qint64 runtimeTicks = 0;
    // The expected size in bytes, -1 when the server sent none
    qint64 size = -1;
    qint64 bytesWritten = 0;
    // The local resume position and whether the item was watched from the local file
    qint64 positionTicks = 0;
    bool played = false;
    qint64 addedAt = 0;
    qint64 updatedAt = 0;
    // The rule that added the item, empty for a manual download
    QString ruleId;
    // What the user wants: false while paused and after a cancel
    bool wanted = true;
    // A manual start on a metered network
    bool meteredOverride = false;

    bool operator==(const DownloadRecord &) const = default;
    // serverId/userId/itemId, lower case
    QString key() const;
};

// The queue of one profile: `<data>/downloads/index.json`, a versioned, atomic 0600 file of at most `MaxFileBytes` and `MaxRecords` records. A file of a later version, an oversized one or one that is not JSON is kept aside as `.broken`; a record that fails validation is dropped alone, so a damaged entry never hides the others. Ids only in keys and paths; server text stays capped. Writes are debounced unless the caller asks for one now
class DownloadStore final : public QObject {
    Q_OBJECT

  public:
    static constexpr int FileVersion = 1;
    static constexpr int MaxRecords = 2000;
    // A realistic record serializes to about 0.8 KB and the worst valid one to about 3.5 KB, so 4 MiB leaves the record cap as the one that binds in practice
    static constexpr qint64 MaxFileBytes = 4 * 1024 * 1024;

    explicit DownloadStore(QObject *parent = nullptr);
    ~DownloadStore() override;

    // Empty, the default: nothing is read or written and the records live in memory. A path reads it at once, flushing the previous file's pending write first
    void setStoragePath(const QString &path);
    QString storagePath() const;
    // The directory the media directories sit under: the storage file's directory
    QString rootDirectory() const;

    QList<DownloadRecord> records() const;
    std::optional<DownloadRecord> find(const QString &serverId, const QString &userId,
                                       const QString &itemId) const;
    std::optional<DownloadRecord> findByKey(const QString &key) const;
    // Inserts or replaces by key and schedules the write; `writeNow` writes before returning. False for an invalid record, a new record when `MaxRecords` are stored (completed and exported ones are never dropped to make room) or a change that would grow the file past `MaxFileBytes`, and then the previous record stays as it was; a failed write keeps the change in memory and reports saveFailed()
    bool upsert(const DownloadRecord &record, bool writeNow = true);
    bool remove(const QString &serverId, const QString &userId, const QString &itemId,
                bool writeNow = true);
    // Writes what is pending now
    void flush();

  Q_SIGNALS:
    // A write failed (once per streak)
    void saveFailed();
    // A write after a failed streak succeeded
    void saveSucceeded();

  private:
    void load();
    void scheduleSave();
    QByteArray serialize() const;
    bool write();

    QString m_path;
    QList<DownloadRecord> m_records;
    QTimer m_saveTimer;
    // The exact serialized size of the records and separators, so a cap check does not rebuild the document per insert
    qint64 m_bytes = 0;
    bool m_dirty = false;
    bool m_saveFailing = false;
};
} // namespace Kinefin

Q_DECLARE_METATYPE(Kinefin::DownloadRecord)
