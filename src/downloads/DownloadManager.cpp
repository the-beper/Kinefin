// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "DownloadManager.h"

#include "api/MediaSourceSelection.h"
#include "downloads/DownloadTransfer.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <cstdio>
#include <utility>

namespace Kinefin {
namespace {
constexpr qint64 MaxTimestamp = qint64(1) << 52;
// A transfer with no progress for this long is a stalled one: its speed starts to fall, a row at a time, until the window is empty
constexpr int StallMs = 1000;

qint64 now()
{
    return std::min(QDateTime::currentMSecsSinceEpoch(), MaxTimestamp);
}

bool validContainer(const QString &container)
{
    if (container.size() < 1 || container.size() > DownloadRecord::MaxContainer)
        return false;
    return std::all_of(container.cbegin(), container.cend(), [](QChar c) {
        const char16_t u = c.unicode();
        return (u >= u'0' && u <= u'9') || (u >= u'a' && u <= u'z') || (u >= u'A' && u <= u'Z');
    });
}

// The server sends a comma-separated list of containers; the first one is the file extension, anything unusable falls back to bin
QString downloadContainer(const QString &container)
{
    const QString first =
        container.split(QLatin1Char(','), Qt::SkipEmptyParts).value(0).trimmed().toLower();
    return validContainer(first) ? first : QStringLiteral("bin");
}

// The order a queue starts in: the oldest request first
bool olderFirst(const DownloadRecord &left, const DownloadRecord &right)
{
    if (left.addedAt != right.addedAt)
        return left.addedAt < right.addedAt;
    return left.key() < right.key();
}
} // namespace

DownloadManager::DownloadManager(QObject *parent) : QObject(parent)
{
    m_clock.start();
    m_speedTimer.setInterval(StallMs);
    QObject::connect(&m_store, &DownloadStore::saveFailed, this, &DownloadManager::storageError);
    QObject::connect(&m_speedTimer, &QTimer::timeout, this, &DownloadManager::decaySpeeds);
}

DownloadManager::~DownloadManager()
{
    for (DownloadTransfer *transfer : std::as_const(m_active))
        delete transfer;
    m_active.clear();
    m_store.flush();
}

void DownloadManager::setStoragePath(const QString &indexPath)
{
    if (indexPath == m_store.storagePath())
        return;
    for (DownloadTransfer *transfer : std::as_const(m_active))
        delete transfer;
    m_active.clear();
    m_speeds.clear();
    m_speedTimer.stop();
    // A removal still waiting for its transfer to stop completes on the old profile: the transfers are gone, so nothing writes the files any more
    for (auto removing = m_removing.cbegin(); removing != m_removing.cend(); ++removing) {
        const std::optional<DownloadRecord> found = m_store.findByKey(removing.key());
        if (!found || removing.value() != m_generation.value(removing.key()))
            continue;
        dropFiles(*found);
        m_store.remove(found->serverId, found->userId, found->itemId, true);
    }
    m_stopping.clear();
    m_removing.clear();
    m_generation.clear();
    m_retryAttempt.clear();
    m_retryPending.clear();
    for (DownloadRecord record : m_store.records()) {
        if (record.state != QLatin1String("fetching"))
            continue;
        record.state = QStringLiteral("queued");
        record.updatedAt = now();
        m_store.upsert(record, false);
    }
    m_store.setStoragePath(indexPath);
    for (DownloadRecord record : m_store.records()) {
        if (record.state != QLatin1String("fetching"))
            continue;
        record.state = QStringLiteral("queued");
        record.updatedAt = now();
        m_store.upsert(record, true);
    }
    Q_EMIT recordsChanged();
    pump();
}

QString DownloadManager::storagePath() const
{
    return m_store.storagePath();
}

DownloadStore *DownloadManager::store()
{
    return &m_store;
}

const DownloadStore *DownloadManager::store() const
{
    return &m_store;
}

void DownloadManager::setParallelDownloads(int count)
{
    m_parallel = std::clamp(count, 1, MaxParallel);
    pump();
}

int DownloadManager::parallelDownloads() const
{
    return m_parallel;
}

void DownloadManager::setOnline(bool online)
{
    if (m_online == online)
        return;
    m_online = online;
    if (!online)
        stopAll(Stop::Hold);
    pump();
}

bool DownloadManager::online() const
{
    return m_online;
}

void DownloadManager::setSourceResolver(SourceResolver resolver)
{
    m_resolver = std::move(resolver);
    pump();
}

bool DownloadManager::enqueue(const DownloadRecord &record, bool batch)
{
    DownloadRecord stored = record;
    stored.container = downloadContainer(record.container);
    stored.state = QStringLiteral("queued");
    stored.error.clear();
    stored.httpStatus = 0;
    stored.wanted = true;
    stored.updatedAt = now();
    if (stored.addedAt <= 0)
        stored.addedAt = stored.updatedAt;
    if (!m_store.upsert(stored, !batch))
        return false;
    ++m_generation[stored.key()];
    const std::optional<DownloadRecord> saved =
        m_store.find(stored.serverId, stored.userId, stored.itemId);
    if (saved)
        Q_EMIT recordChanged(*saved);
    Q_EMIT recordsChanged();
    if (!batch)
        pump();
    return true;
}

void DownloadManager::wake()
{
    pump();
}

bool DownloadManager::updateRecord(const DownloadRecord &record)
{
    const std::optional<DownloadRecord> found = m_store.findByKey(record.key());
    if (!found)
        return false;
    DownloadRecord updated = record;
    updated.addedAt = found->addedAt;
    updated.updatedAt = now();
    // The transfer owns these while it runs; a stale copy from a caller must not rewind them
    if (m_active.contains(found->key())) {
        updated.state = found->state;
        updated.error = found->error;
        updated.httpStatus = found->httpStatus;
        updated.wanted = found->wanted;
        updated.bytesWritten = found->bytesWritten;
        updated.size = found->size;
        updated.container = found->container;
        updated.etag = found->etag;
    }
    if (!m_store.upsert(updated, true))
        return false;
    const std::optional<DownloadRecord> saved = m_store.findByKey(found->key());
    Q_EMIT recordChanged(saved.value_or(updated));
    return true;
}

bool DownloadManager::setState(const QString &serverId, const QString &userId,
                               const QString &itemId, const QString &state)
{
    const std::optional<DownloadRecord> found = m_store.find(serverId, userId, itemId);
    if (!found)
        return false;
    DownloadRecord record = *found;
    record.state = state;
    record.updatedAt = now();
    if (!m_store.upsert(record, true))
        return false;
    Q_EMIT recordChanged(record);
    Q_EMIT recordsChanged();
    return true;
}

bool DownloadManager::retry(const QString &serverId, const QString &userId, const QString &itemId)
{
    const std::optional<DownloadRecord> found = m_store.find(serverId, userId, itemId);
    if (!found)
        return false;
    const QString key = found->key();
    if (found->state == QLatin1String("completed") || found->state == QLatin1String("exported")
        || found->state == QLatin1String("fetching"))
        return false;
    m_retryAttempt.remove(key);
    m_retryPending.remove(key);
    DownloadRecord record = *found;
    record.state = QStringLiteral("queued");
    record.error.clear();
    record.httpStatus = 0;
    record.wanted = true;
    record.updatedAt = now();
    if (!m_store.upsert(record, true))
        return false;
    Q_EMIT recordChanged(record);
    Q_EMIT recordsChanged();
    pump();
    return true;
}

bool DownloadManager::pause(const QString &serverId, const QString &userId, const QString &itemId)
{
    const std::optional<DownloadRecord> found = m_store.find(serverId, userId, itemId);
    if (!found || found->state == QLatin1String("completed")
        || found->state == QLatin1String("exported"))
        return false;
    DownloadRecord record = *found;
    record.state = QStringLiteral("paused");
    record.error.clear();
    record.httpStatus = 0;
    record.wanted = false;
    record.updatedAt = now();
    if (!m_store.upsert(record, true))
        return false;
    Q_EMIT recordChanged(record);
    stopActive(record.key(), Stop::Pause);
    return true;
}

bool DownloadManager::remove(const QString &serverId, const QString &userId, const QString &itemId)
{
    const std::optional<DownloadRecord> found = m_store.find(serverId, userId, itemId);
    if (!found)
        return true;
    const QString key = found->key();
    if (m_active.contains(key)) {
        m_removing.insert(key, m_generation.value(key));
        m_stopping.insert(key, Stop::Remove);
        m_active.value(key)->cancel();
        return true;
    }
    dropFiles(*found);
    if (!m_store.remove(serverId, userId, itemId, true))
        return false;
    m_retryAttempt.remove(key);
    m_retryPending.remove(key);
    Q_EMIT downloadRemoved(serverId, userId, itemId);
    Q_EMIT recordsChanged();
    pump();
    return true;
}

QString DownloadManager::localPath(const DownloadRecord &record) const
{
    if (record.state != QLatin1String("completed") && record.state != QLatin1String("exported"))
        return {};
    const QString path = targetPath(record);
    const QFileInfo info(path);
    if (path.isEmpty() || !info.isFile() || info.isSymLink()
        || (record.size >= 0 && info.size() != record.size))
        return {};
    return path;
}

QString DownloadManager::localPath(const QString &serverId, const QString &userId,
                                   const QString &itemId) const
{
    const std::optional<DownloadRecord> found = m_store.find(serverId, userId, itemId);
    return found ? localPath(*found) : QString();
}

void DownloadManager::setRetryDelays(const QList<int> &milliseconds)
{
    m_retryDelays = milliseconds;
    m_retryAttempt.clear();
    m_retryPending.clear();
}

QList<int> DownloadManager::retryDelays() const
{
    return m_retryDelays;
}

qint64 DownloadManager::bytesPerSecond(const DownloadRecord &record) const
{
    return record.state == QLatin1String("fetching") ? speedOf(record.key()) : 0;
}

qint64 DownloadManager::totalBytesPerSecond() const
{
    qint64 total = 0;
    for (auto speed = m_speeds.cbegin(); speed != m_speeds.cend(); ++speed)
        total += speedOf(speed.key());
    return total;
}

qint64 DownloadManager::speedOf(const QString &key) const
{
    // A transfer being stopped has no speed any more, whatever its last samples say
    const auto found = m_speeds.constFind(key);
    if (found == m_speeds.cend() || m_stopping.contains(key))
        return 0;
    return found->bytesPerSecond(m_clock.elapsed());
}

void DownloadManager::decaySpeeds()
{
    const qint64 at = m_clock.elapsed();
    QStringList stalled;
    for (auto speed = m_speeds.begin(); speed != m_speeds.end(); ++speed) {
        if (speed->empty() || at - speed->lastSampleMs() < StallMs)
            continue;
        stalled.append(speed.key());
        // The last report of a spent window reads zero
        if (at - speed->lastSampleMs() >= SpeedAverage::WindowMs)
            speed->clear();
    }
    for (const QString &key : std::as_const(stalled)) {
        const std::optional<DownloadRecord> found = m_store.findByKey(key);
        if (found)
            Q_EMIT recordChanged(*found);
    }
}

void DownloadManager::pump()
{
    if (!m_online)
        return;
    QList<DownloadRecord> records = m_store.records();
    std::sort(records.begin(), records.end(), olderFirst);
    bool held = false;
    for (const DownloadRecord &record : std::as_const(records)) {
        if (m_active.size() >= m_parallel)
            break;
        const QString key = record.key();
        if (m_active.contains(key) || m_stopping.contains(key) || !record.wanted)
            continue;
        if (record.state != QLatin1String("queued") && record.state != QLatin1String("waiting"))
            continue;
        // A whole pass over the queue holds its records with one write
        if (!m_resolver) {
            held = setRecordState(key, QStringLiteral("waiting"), {}, 0, false) || held;
            continue;
        }
        std::optional<DownloadSource> source = m_resolver(record);
        if (!source) {
            held = setRecordState(key, QStringLiteral("waiting"), {}, 0, false) || held;
            continue;
        }
        start(record, *source);
    }
    if (held)
        m_store.flush();
}

void DownloadManager::start(const DownloadRecord &record, const DownloadSource &source)
{
    const QString key = record.key();
    const QString target = targetPath(record);
    const QString partial = partialPath(record);
    const QString sidecar = sidecarPath(record);
    if (target.isEmpty() || partial.isEmpty() || sidecar.isEmpty()) {
        setRecordState(key, QStringLiteral("error"), QStringLiteral("io"));
        return;
    }
    DownloadTransfer::Request request;
    request.url = source.url;
    request.authorization = source.authorization;
    request.targetPath = target;
    request.partialPath = partial;
    request.sidecarPath = sidecar;
    request.resumeKey = resolveKey(record);
    request.expectedSize = record.size;
    request.etag = record.etag;
    request.ignoreCertificateErrors = source.ignoreCertificateErrors;

    auto *transfer = new DownloadTransfer(this);
    QObject::connect(transfer, &DownloadTransfer::progress, this,
                     [this, key, transfer](qint64 written, qint64 total) {
                         if (m_active.value(key) != transfer)
                             return;
                         const std::optional<DownloadRecord> found = m_store.findByKey(key);
                         if (!found)
                             return;
                         DownloadRecord updated = *found;
                         updated.bytesWritten = written;
                         if (total >= 0)
                             updated.size = total;
                         updated.updatedAt = now();
                         m_store.upsert(updated, false);
                         m_speeds[key].add(m_clock.elapsed(), written);
                         // Progress is a row update, not a structural change: the page keeps its scroll and focus
                         Q_EMIT recordChanged(updated);
                     });
    QObject::connect(transfer, &DownloadTransfer::finished, this,
                     [this, key, transfer](qint64 size, const QString &etag) {
                         if (m_active.value(key) != transfer)
                             return;
                         release(key);
                         finishTransfer(key, true, size, etag, 0, 0);
                         pump();
                     });
    QObject::connect(transfer, &DownloadTransfer::failed, this,
                     [this, key, transfer](DownloadTransfer::Failure failure, int httpStatus) {
                         if (m_active.value(key) != transfer)
                             return;
                         release(key);
                         finishTransfer(key, false, 0, {}, int(failure), httpStatus);
                         pump();
                     });
    DownloadRecord updated = record;
    updated.state = QStringLiteral("fetching");
    updated.error.clear();
    updated.httpStatus = 0;
    updated.wanted = true;
    updated.updatedAt = now();
    // A full queue that cannot record the start does not fetch: the file would have no record to complete
    if (!m_store.upsert(updated, true)) {
        transfer->deleteLater();
        return;
    }
    if (!transfer->start(request)) {
        transfer->deleteLater();
        setRecordState(key, QStringLiteral("error"), QStringLiteral("io"));
        return;
    }
    m_active.insert(key, transfer);
    m_speeds.remove(key);
    if (!m_speedTimer.isActive())
        m_speedTimer.start();
    Q_EMIT recordChanged(updated);
    Q_EMIT recordsChanged();
}

void DownloadManager::release(const QString &key)
{
    DownloadTransfer *transfer = m_active.take(key);
    if (transfer)
        transfer->deleteLater();
    m_speeds.remove(key);
    if (m_active.isEmpty())
        m_speedTimer.stop();
}

void DownloadManager::finishTransfer(const QString &key, bool completed, qint64 size,
                                     const QString &etag, int failure, int httpStatus)
{
    const std::optional<DownloadRecord> found = m_store.findByKey(key);
    if (!found)
        return;
    const Stop stop = m_stopping.take(key);
    DownloadRecord record = *found;
    // A write the store refuses leaves the record `fetching` with no transfer, which the queue never takes again: it becomes an io error instead, and the storage error is reported
    const auto commit = [this, &found](const DownloadRecord &next) {
        if (m_store.upsert(next, true))
            return true;
        Q_EMIT storageError();
        DownloadRecord failed = *found;
        failed.state = QStringLiteral("error");
        failed.error = QStringLiteral("io");
        failed.httpStatus = 0;
        failed.wanted = true;
        failed.updatedAt = now();
        if (m_store.upsert(failed, true)) {
            Q_EMIT recordChanged(failed);
            Q_EMIT recordsChanged();
        }
        return false;
    };
    if (stop == Stop::Remove) {
        // A record enqueued again while the old transfer was stopping is a new generation and keeps its files
        if (m_removing.take(key) != m_generation.value(key))
            return;
        dropFiles(record);
        m_store.remove(record.serverId, record.userId, record.itemId, true);
        m_retryAttempt.remove(key);
        m_retryPending.remove(key);
        Q_EMIT downloadRemoved(record.serverId, record.userId, record.itemId);
        Q_EMIT recordsChanged();
        return;
    }
    if (completed) {
        record.state = QStringLiteral("completed");
        record.size = size;
        record.bytesWritten = size;
        if (!etag.isEmpty())
            record.etag = etag.left(DownloadRecord::MaxEtag);
        record.error.clear();
        record.httpStatus = 0;
        record.wanted = true;
        record.updatedAt = now();
        if (!commit(record))
            return;
        m_retryAttempt.remove(key);
        m_retryPending.remove(key);
        Q_EMIT recordChanged(record);
        Q_EMIT downloadCompleted(record);
        Q_EMIT recordsChanged();
        return;
    }
    if (stop == Stop::Pause) {
        if (record.wanted || record.state != QLatin1String("paused"))
            return;
        record.error.clear();
        record.updatedAt = now();
        if (!commit(record))
            return;
        Q_EMIT recordChanged(record);
        Q_EMIT recordsChanged();
        return;
    }
    if (stop == Stop::Hold) {
        if (!record.wanted)
            return;
        // The gate may have opened again before this failure arrived: requeue, and the caller's pump restarts it
        record.state = m_online ? QStringLiteral("queued") : QStringLiteral("waiting");
        record.error.clear();
        record.httpStatus = 0;
        record.updatedAt = now();
        if (!commit(record))
            return;
        Q_EMIT recordChanged(record);
        Q_EMIT recordsChanged();
        return;
    }
    record.state = QStringLiteral("error");
    record.error = failureToken(failure);
    record.httpStatus = httpStatus;
    record.wanted = true;
    record.updatedAt = now();
    if (!commit(record))
        return;
    Q_EMIT recordChanged(record);
    Q_EMIT recordsChanged();
    scheduleRetry(key, httpStatus);
}

bool DownloadManager::setRecordState(const QString &key, const QString &state, const QString &error,
                                     int httpStatus, bool writeNow)
{
    const std::optional<DownloadRecord> found = m_store.findByKey(key);
    if (!found || (found->state == state && found->error == error))
        return false;
    DownloadRecord record = *found;
    record.state = state;
    record.error = error;
    record.httpStatus = httpStatus;
    record.updatedAt = now();
    m_store.upsert(record, writeNow);
    Q_EMIT recordChanged(record);
    Q_EMIT recordsChanged();
    return true;
}

void DownloadManager::dropFiles(const DownloadRecord &record)
{
    // targetPath() has validated the ids; the folder must also be the real per-item one under the root, never a link out of it
    const QString target = targetPath(record);
    const QString root = QDir(m_store.rootDirectory()).canonicalPath();
    if (target.isEmpty() || root.isEmpty())
        return;
    const QFileInfo directory(QFileInfo(target).absolutePath());
    const QString owner = root + QLatin1Char('/') + record.serverId.toLower() + QLatin1Char('/')
                          + record.userId.toLower();
    if (!directory.isDir() || directory.isSymLink()
        || directory.canonicalFilePath() != owner + QLatin1Char('/') + record.itemId.toLower())
        return;
    // The folder moves to a name of its own before it is removed: a link swapped in after the check above is removed as a link, never followed; QDir::removeRecursively removes a link inside the folder without following it
    const QString tombstone =
        owner + QStringLiteral("/.removing-") + QUuid::createUuid().toString(QUuid::Id128);
    if (::rename(QFile::encodeName(directory.absoluteFilePath()).constData(),
                 QFile::encodeName(tombstone).constData())
        != 0)
        return;
    const QFileInfo moved(tombstone);
    if (moved.isSymLink())
        QFile::remove(tombstone);
    else if (moved.isDir())
        QDir(tombstone).removeRecursively();
    // Only an empty folder goes: another item of the same owner keeps its parents
    QDir().rmdir(owner);
    QDir().rmdir(QFileInfo(owner).absolutePath());
}

void DownloadManager::scheduleRetry(const QString &key, int httpStatus)
{
    const std::optional<DownloadRecord> found = m_store.findByKey(key);
    if (!found || found->ruleId.isEmpty() || m_retryPending.contains(key))
        return;
    const bool retryable =
        found->error == QLatin1String("network")
        || (found->error == QLatin1String("http") && httpStatus >= 500 && httpStatus <= 599);
    if (!retryable)
        return;
    const int attempt = m_retryAttempt.value(key, 0);
    if (attempt >= m_retryDelays.size())
        return;
    m_retryAttempt.insert(key, attempt + 1);
    m_retryPending.insert(key);
    QTimer::singleShot(m_retryDelays.at(attempt), this, [this, key] {
        if (!m_retryPending.remove(key))
            return;
        const std::optional<DownloadRecord> record = m_store.findByKey(key);
        if (!record || record->state != QLatin1String("error") || record->ruleId.isEmpty())
            return;
        DownloadRecord queued = *record;
        queued.state = QStringLiteral("queued");
        queued.error.clear();
        queued.httpStatus = 0;
        queued.updatedAt = now();
        if (!m_store.upsert(queued, true))
            return;
        Q_EMIT recordChanged(queued);
        Q_EMIT recordsChanged();
        pump();
    });
}

void DownloadManager::stopActive(const QString &key, Stop reason)
{
    DownloadTransfer *transfer = m_active.value(key);
    if (!transfer) {
        const std::optional<DownloadRecord> found = m_store.findByKey(key);
        if (!found)
            return;
        if (reason == Stop::Pause)
            setRecordState(key, QStringLiteral("paused"));
        else if (reason == Stop::Hold)
            setRecordState(key, QStringLiteral("waiting"));
        else {
            dropFiles(*found);
            m_store.remove(found->serverId, found->userId, found->itemId, true);
            Q_EMIT downloadRemoved(found->serverId, found->userId, found->itemId);
            Q_EMIT recordsChanged();
        }
        return;
    }
    // A removal outranks a pause and a pause outranks a hold: a later, weaker reason never replaces the pending one
    const auto rank = [](Stop value) {
        switch (value) {
        case Stop::Remove:
            return 3;
        case Stop::Pause:
            return 2;
        case Stop::Hold:
            return 1;
        case Stop::None:
            break;
        }
        return 0;
    };
    const auto pending = m_stopping.constFind(key);
    if (pending != m_stopping.constEnd()) {
        if (rank(reason) > rank(*pending))
            m_stopping.insert(key, reason);
        return;
    }
    m_stopping.insert(key, reason);
    transfer->cancel();
}

void DownloadManager::stopAll(Stop reason)
{
    const QList<QString> keys = m_active.keys();
    for (const QString &key : keys)
        stopActive(key, reason);
}

QString DownloadManager::failureToken(int failure)
{
    switch (failure) {
    case int(DownloadTransfer::Failure::Network):
        return QStringLiteral("network");
    case int(DownloadTransfer::Failure::Http):
        return QStringLiteral("http");
    case int(DownloadTransfer::Failure::Protocol):
        return QStringLiteral("protocol");
    case int(DownloadTransfer::Failure::SizeMismatch):
        return QStringLiteral("size");
    case int(DownloadTransfer::Failure::Changed):
        return QStringLiteral("changed");
    case int(DownloadTransfer::Failure::Io):
        return QStringLiteral("io");
    case int(DownloadTransfer::Failure::NoSpace):
        return QStringLiteral("nospace");
    case int(DownloadTransfer::Failure::Cancelled):
        return QStringLiteral("cancelled");
    }
    return QStringLiteral("protocol");
}

QString DownloadManager::targetPath(const DownloadRecord &record) const
{
    if (!isValidJellyfinId(record.serverId) || !isValidJellyfinId(record.userId)
        || !isValidJellyfinId(record.itemId) || !validContainer(record.container))
        return {};
    const QString root = m_store.rootDirectory();
    if (root.isEmpty())
        return {};
    return root + QLatin1Char('/') + record.serverId.toLower() + QLatin1Char('/')
           + record.userId.toLower() + QLatin1Char('/') + record.itemId.toLower()
           + QStringLiteral("/media.") + record.container;
}

QString DownloadManager::partialPath(const DownloadRecord &record) const
{
    const QString target = targetPath(record);
    return target.isEmpty() ? QString()
                            : QFileInfo(target).absolutePath() + QStringLiteral("/media.part");
}

QString DownloadManager::sidecarPath(const DownloadRecord &record) const
{
    const QString target = targetPath(record);
    return target.isEmpty()
               ? QString()
               : QFileInfo(target).absolutePath() + QStringLiteral("/media.kinefin-part.json");
}

QString DownloadManager::resolveKey(const DownloadRecord &record) const
{
    return record.itemId.toLower();
}
} // namespace Kinefin
