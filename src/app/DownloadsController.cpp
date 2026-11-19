// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "DownloadsController.h"

#include "api/MediaSourceSelection.h"
#include "downloads/ExportNames.h"
#include "imagecache/ImageNetworkAccess.h"
#include "models/ItemDetails.h"
#include "settings/ProfilePaths.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QThread>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>

Q_LOGGING_CATEGORY(downloadsLog, "kinefin.downloads")

namespace Kinefin {
namespace {
const QStringList kDownloadableTypes{QStringLiteral("Movie"), QStringLiteral("Episode"),
                                     QStringLiteral("Video")};

// The store refuses control characters and overlong server text: clean what it would reject instead of failing the download for a title
QString cleanText(const QString &text, int max)
{
    QString cleaned = text;
    for (QChar &character : cleaned) {
        const char16_t value = character.unicode();
        if (value < 0x20 || (value >= 0x7f && value <= 0x9f) || value == 0x2028 || value == 0x2029)
            character = QLatin1Char(' ');
    }
    return cleaned.simplified().left(max);
}

// A poster is a 160 px image: anything larger is not one
constexpr qint64 kMaxPosterBytes = 2 * 1024 * 1024;

// The server writes up to seven fractional digits: the cursor keeps the millisecond ones
QDateTime parseServerDate(const QString &text)
{
    static const QRegularExpression fraction(QStringLiteral("(\\.\\d{3})\\d+"));
    QString trimmed = text;
    trimmed.replace(fraction, QStringLiteral("\\1"));
    return QDateTime::fromString(trimmed, Qt::ISODateWithMs);
}

bool pathTaken(const QString &path)
{
    // A dangling symlink does not exist as far as exists() is concerned, but creating through it would follow it
    const QFileInfo info(path);
    return info.exists() || info.isSymLink();
}

// The first `name`, `name (2)`... below `directory` for which the media file, its partial and the sidecars are all free
QString freeExportBase(const QString &directory, const QString &name, const QString &suffix,
                       bool sidecars, bool poster)
{
    for (int attempt = 1; attempt < 1000; ++attempt) {
        const QString base = attempt == 1 ? name : QStringLiteral("%1 (%2)").arg(name).arg(attempt);
        const QString stem = directory + QLatin1Char('/') + base;
        const QString media = stem + QLatin1Char('.') + suffix;
        if (pathTaken(media) || pathTaken(media + QStringLiteral(".part")))
            continue;
        if (sidecars
            && (pathTaken(stem + QStringLiteral(".nfo"))
                || (poster && pathTaken(stem + QStringLiteral("-poster.jpg")))))
            continue;
        return base;
    }
    return {};
}

// Copies `source` to a file that did not exist (never through a symlink); a partial destination is removed again on failure
// QFile::rename falls back to a copy that truncates the target and follows a link planted there; renameat2 never replaces, and where the filesystem lacks the flag (the document portal's FUSE mount may) a plain rename replaces the entry itself, never following it
bool renameNoReplace(const QString &from, const QString &to)
{
    const QByteArray source = QFile::encodeName(from);
    const QByteArray target = QFile::encodeName(to);
    if (::renameat2(AT_FDCWD, source.constData(), AT_FDCWD, target.constData(), RENAME_NOREPLACE)
        == 0)
        return true;
    if (errno != EINVAL && errno != ENOSYS)
        return false;
    const QFileInfo existing(to);
    if (existing.exists() || existing.isSymLink())
        return false;
    return ::rename(source.constData(), target.constData()) == 0;
}

bool copyNewFile(const QString &source, const QString &destination)
{
    QFile input(source);
    QFile output(destination);
    if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return false;
    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    for (;;) {
        const qint64 read = input.read(buffer.data(), buffer.size());
        if (read < 0 || (read > 0 && output.write(buffer.constData(), read) != read)) {
            output.close();
            QFile::remove(destination);
            return false;
        }
        if (read == 0)
            break;
    }
    if (!output.flush()) {
        output.close();
        QFile::remove(destination);
        return false;
    }
    return true;
}

QString failureForStatus(int httpStatus)
{
    if (httpStatus == 401 || httpStatus == 403)
        return QStringLiteral("forbidden");
    if (httpStatus == 404)
        return QStringLiteral("notfound");
    if (httpStatus == 0)
        return QStringLiteral("network");
    return QStringLiteral("failed");
}
} // namespace

DownloadsController::DownloadsController(JellyfinClient *client, const QString &profileId,
                                         QObject *parent)
    : QObject(parent), m_client(client)
{
    // Connected before the first load: a record the load rewrites can fail to save too
    connect(&m_manager, &DownloadManager::storageError, this, [this] {
        qCWarning(downloadsLog) << "The download list could not be saved";
        if (m_storageFailed)
            return;
        m_storageFailed = true;
        Q_EMIT storageFailedChanged();
    });
    connect(m_manager.store(), &DownloadStore::saveSucceeded, this, [this] {
        if (!m_storageFailed)
            return;
        m_storageFailed = false;
        Q_EMIT storageFailedChanged();
    });
    // The index lives in the profile's data folder; an invalid profile id keeps the queue in memory only
    const QString dataDirectory = profileDataDirectory(profileId);
    if (!dataDirectory.isEmpty()) {
        const QString directory = dataDirectory + QStringLiteral("/downloads");
        if (ensurePrivateDirectory(directory)) {
            m_manager.setStoragePath(directory + QStringLiteral("/index.json"));
            m_outbox.setStoragePath(directory + QStringLiteral("/outbox.json"));
            m_rules.setStoragePath(directory + QStringLiteral("/rules.json"));
        } else
            qCWarning(downloadsLog) << "Downloads are kept in memory: the profile's downloads "
                                       "directory could not be created";
    }
    m_manager.setSourceResolver([this](const DownloadRecord &record) {
        // Another user's record waits for that user: its URL is built from the signed-in session, never from an old owner
        if (record.serverId.compare(m_client->serverId(), Qt::CaseInsensitive) != 0
            || record.userId.compare(m_client->userId(), Qt::CaseInsensitive) != 0)
            return std::optional<DownloadSource>();
        const QUrl url = m_client->downloadUrl(record.itemId);
        if (url.isEmpty() || !m_client->canDownload())
            return std::optional<DownloadSource>();
        DownloadSource source;
        source.url = url;
        source.authorization = m_client->streamAuthorization();
        source.ignoreCertificateErrors = m_ignoreCertificateErrors;
        return std::optional<DownloadSource>(source);
    });
    m_manager.setOnline(false);
    m_manager.setMetered(m_meter.metered() && m_unmeteredOnly);
    connect(&m_meter, &NetworkMeter::meteredChanged, this, [this](bool metered) {
        m_manager.setMetered(metered && m_unmeteredOnly);
        Q_EMIT meteredChanged();
    });
    connect(m_client, &JellyfinClient::itemsReady, this, &DownloadsController::onItemsReady);
    connect(m_client, &JellyfinClient::itemPlayedUpdated, this,
            [this](const QString &itemId, bool played) {
                if (!m_outboxWrites.remove(itemId) || !played)
                    return;
                // Verify by re-reading: the write may have landed on a server that kept its own state
                for (const WatchedEvent &event : m_outbox.events()) {
                    if (event.itemId.compare(itemId, Qt::CaseInsensitive) != 0
                        || event.serverId.compare(m_client->serverId(), Qt::CaseInsensitive) != 0
                        || event.userId.compare(m_client->userId(), Qt::CaseInsensitive) != 0)
                        continue;
                    ItemQuery query;
                    query.itemIds = {event.itemId};
                    const int channel = m_nextChannel++;
                    if (!m_client->fetchItems(query, channel)) {
                        reconcileNext();
                        return;
                    }
                    m_outboxVerifies.insert(channel, event.key());
                    return;
                }
                reconcileNext();
            });
    connect(m_client, &JellyfinClient::itemPlayedUpdateFailed, this,
            [this](const QString &itemId, bool, int) { m_outboxWrites.remove(itemId); });
    connect(&m_outbox, &WatchedOutbox::changed, this, &DownloadsController::outboxChanged);
    connect(&m_rules, &DownloadRules::changed, this, [this] {
        ++m_rulesRevision;
        Q_EMIT rulesRevisionChanged();
    });
    connect(&m_rules, &DownloadRules::saveFailed, this,
            [this] { qCWarning(downloadsLog) << "The download rules could not be saved"; });
    connect(m_client, &JellyfinClient::episodesReady, this, &DownloadsController::onEpisodesReady);
    m_ruleTimer.setInterval(6 * 60 * 60 * 1000);
    connect(&m_ruleTimer, &QTimer::timeout, this, &DownloadsController::scanRules);
    m_ruleTimer.start();
    m_ruleDebounce.setSingleShot(true);
    m_ruleDebounce.setInterval(5000);
    connect(&m_ruleDebounce, &QTimer::timeout, this, &DownloadsController::scanRules);
    connect(&m_outbox, &WatchedOutbox::saveFailed, this,
            [this] { qCWarning(downloadsLog) << "The watched-state outbox could not be saved"; });
    connect(m_client, &JellyfinClient::listingFailed, this, &DownloadsController::onListingFailed);
    connect(m_client, &JellyfinClient::loginSucceeded, this, [this] {
        refreshOwner();
        refreshCanDownload();
    });
    connect(m_client, &JellyfinClient::userProfileRefreshed, this,
            [this] { refreshCanDownload(); });
    connect(m_client, &JellyfinClient::credentialsCleared, this, [this] {
        for (auto it = m_fetches.cbegin(); it != m_fetches.cend(); ++it)
            m_client->releaseListingChannel(it.key());
        for (auto it = m_outboxFetches.cbegin(); it != m_outboxFetches.cend(); ++it)
            m_client->releaseListingChannel(it.key());
        for (auto it = m_outboxVerifies.cbegin(); it != m_outboxVerifies.cend(); ++it)
            m_client->releaseListingChannel(it.key());
        for (auto it = m_ruleFetches.cbegin(); it != m_ruleFetches.cend(); ++it)
            m_client->releaseListingChannel(it.key());
        m_fetches.clear();
        m_fetchRules.clear();
        m_fetchRuleDates.clear();
        m_fetching.clear();
        m_outboxFetches.clear();
        m_outboxVerifies.clear();
        m_outboxWrites.clear();
        m_ruleFetches.clear();
        m_ruleItems.clear();
        refreshOwner();
        refreshCanDownload();
    });
    connect(&m_manager, &DownloadManager::downloadCompleted, this, &DownloadsController::writeInfo);
    connect(&m_manager, &DownloadManager::recordsChanged, this, [this] {
        ++m_revision;
        Q_EMIT revisionChanged();
    });
    connect(&m_manager, &DownloadManager::recordChanged, this, [this] {
        ++m_revision;
        Q_EMIT revisionChanged();
    });
    refreshOwner();
    refreshCanDownload();
}

DownloadsController::~DownloadsController()
{
    if (m_exportThread) {
        m_exportThread->wait();
        delete m_exportThread;
        m_exportThread = nullptr;
    }
}

bool DownloadsController::canDownload() const
{
    return m_client->canDownload();
}

QString DownloadsController::ownerServerId() const
{
    return m_offlineUserId.isEmpty() ? m_client->serverId() : m_offlineServerId;
}

QString DownloadsController::ownerUserId() const
{
    return m_offlineUserId.isEmpty() ? m_client->userId() : m_offlineUserId;
}

bool DownloadsController::ownedByCurrent(const QString &serverId, const QString &userId) const
{
    const QString owner = ownerUserId();
    return !owner.isEmpty() && serverId.compare(ownerServerId(), Qt::CaseInsensitive) == 0
           && userId.compare(owner, Qt::CaseInsensitive) == 0;
}

void DownloadsController::refreshOwner()
{
    m_model.setOwner(ownerServerId(), ownerUserId());
    const QString key = (ownerServerId() + QLatin1Char('/') + ownerUserId()).toLower();
    if (key == m_ownerKey)
        return;
    m_ownerKey = key;
    Q_EMIT ownerChanged();
    // The conflicts and the pending count follow the owner too
    Q_EMIT outboxChanged();
}

void DownloadsController::setOfflineOwner(const QString &serverId, const QString &userId)
{
    m_offlineServerId = userId.isEmpty() ? QString() : serverId;
    m_offlineUserId = userId;
    refreshOwner();
}

void DownloadsController::setImageOrigin(std::shared_ptr<ImageOrigin> origin)
{
    if (!origin) {
        m_images.reset();
        return;
    }
    // No disk cache: the poster is written next to the media file, and only once per download
    m_images = std::make_unique<ImageNetworkAccess>(std::move(origin));
    m_images->setMaxImageBytes(kMaxPosterBytes);
}

bool DownloadsController::hasLocalFile(const DownloadRecord &record) const
{
    return !m_manager.localPath(record).isEmpty();
}

void DownloadsController::setOnline(bool online)
{
    if (m_online == online)
        return;
    m_online = online;
    m_manager.setOnline(online);
    Q_EMIT onlineChanged();
}

void DownloadsController::setParallelDownloads(int count)
{
    m_manager.setParallelDownloads(count);
}

void DownloadsController::setUnmeteredOnly(bool unmeteredOnly)
{
    m_unmeteredOnly = unmeteredOnly;
    m_manager.setMetered(m_meter.metered() && m_unmeteredOnly);
}

void DownloadsController::setIgnoreCertificateErrors(bool ignore)
{
    m_ignoreCertificateErrors = ignore;
}

void DownloadsController::refreshCanDownload()
{
    const bool allowed = canDownload();
    if (allowed == m_canDownload)
        return;
    m_canDownload = allowed;
    Q_EMIT canDownloadChanged();
    // A restored session learns the policy after loginSucceeded: the records held for it start now
    if (m_canDownload && m_online)
        m_manager.wake();
}

bool DownloadsController::downloadItem(const QString &itemId)
{
    return downloadItemForRule(itemId, {});
}

bool DownloadsController::downloadItemForRule(const QString &itemId, const QString &ruleId,
                                              const QDateTime &created)
{
    if (!canDownload() || !isValidJellyfinId(itemId))
        return false;
    if (m_client->userId().isEmpty())
        return false;
    if (m_manager.store()->find(m_client->serverId(), m_client->userId(), itemId))
        return false;
    if (m_fetching.contains(itemId.toLower()))
        return false;
    ItemQuery query;
    query.itemIds = {itemId};
    query.fields = {QStringLiteral("MediaSources"), QStringLiteral("CanDownload")};
    const int channel = m_nextChannel++;
    if (!m_client->fetchItems(query, channel))
        return false;
    m_fetches.insert(channel, itemId.toLower());
    m_fetchRules.insert(channel, ruleId);
    if (!ruleId.isEmpty() && created.isValid())
        m_fetchRuleDates.insert(channel, created);
    m_fetching.insert(itemId.toLower());
    return true;
}

void DownloadsController::rewindRuleCursor(const QString &seriesId, const QDateTime &created)
{
    if (!created.isValid() || m_client->userId().isEmpty())
        return;
    std::optional<DownloadRule> rule =
        m_rules.find(m_client->serverId(), m_client->userId(), seriesId);
    if (!rule || rule->kind != QLatin1String("newEpisodes"))
        return;
    const QDateTime cursor = parseServerDate(rule->cursorDate);
    if (cursor.isValid() && cursor < created)
        return;
    rule->cursorDate = created.addMSecs(-1).toUTC().toString(Qt::ISODateWithMs);
    rule->cursorId.clear();
    m_rules.upsert(*rule);
}

QString DownloadsController::downloadState(const QString &itemId) const
{
    if (m_client->userId().isEmpty())
        return {};
    const std::optional<DownloadRecord> record =
        m_manager.store()->find(m_client->serverId(), m_client->userId(), itemId);
    return record ? record->state : QString();
}

bool DownloadsController::pauseDownload(const QString &serverId, const QString &userId,
                                        const QString &itemId)
{
    return m_manager.pause(serverId, userId, itemId);
}

bool DownloadsController::retryDownload(const QString &serverId, const QString &userId,
                                        const QString &itemId)
{
    return m_manager.retry(serverId, userId, itemId);
}

bool DownloadsController::removeDownload(const QString &serverId, const QString &userId,
                                         const QString &itemId)
{
    return m_manager.remove(serverId, userId, itemId);
}

bool DownloadsController::removeCurrentDownload(const QString &itemId)
{
    if (m_client->userId().isEmpty())
        return false;
    return m_manager.remove(m_client->serverId(), m_client->userId(), itemId);
}

void DownloadsController::updateLocalProgress(const QString &serverId, const QString &userId,
                                              const QString &itemId, qint64 ticks, bool played)
{
    if (ticks < 0 || ticks > DownloadRecord::MaxRuntimeTicks)
        return;
    const std::optional<DownloadRecord> found = m_manager.store()->find(serverId, userId, itemId);
    if (!found
        || (found->state != QLatin1String("completed")
            && found->state != QLatin1String("exported")))
        return;
    DownloadRecord record = *found;
    // A finished item starts over next time instead of resuming at its end
    record.positionTicks = played ? 0 : ticks;
    if (played) {
        record.played = true;
        WatchedEvent event;
        event.serverId = record.serverId;
        event.userId = record.userId;
        event.itemId = record.itemId;
        event.eventId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        event.createdAt = QDateTime::currentMSecsSinceEpoch();
        event.played = true;
        m_outbox.append(event);
    }
    record.updatedAt = QDateTime::currentMSecsSinceEpoch();
    m_manager.updateRecord(record);
}

void DownloadsController::writeInfo(const DownloadRecord &record)
{
    const QString media = m_manager.localPath(record);
    if (media.isEmpty())
        return;
    const QJsonObject info{{QStringLiteral("name"), record.name},
                           {QStringLiteral("type"), record.type},
                           {QStringLiteral("seriesName"), record.seriesName},
                           {QStringLiteral("seriesId"), record.seriesId},
                           {QStringLiteral("seasonId"), record.seasonId},
                           {QStringLiteral("seasonNumber"), record.seasonNumber},
                           {QStringLiteral("episodeNumber"), record.episodeNumber},
                           {QStringLiteral("year"), record.year},
                           {QStringLiteral("runtimeTicks"), record.runtimeTicks},
                           {QStringLiteral("container"), record.container},
                           {QStringLiteral("size"), record.size},
                           {QStringLiteral("etag"), record.etag},
                           {QStringLiteral("primaryTag"), record.primaryTag},
                           {QStringLiteral("positionTicks"), record.positionTicks},
                           {QStringLiteral("played"), record.played}};
    const QString directory = QFileInfo(media).absolutePath();
    writePrivateFile(directory + QStringLiteral("/info.json"),
                     QJsonDocument(info).toJson(QJsonDocument::Compact));
    const QUrl poster = posterUrl(record.itemId, record.primaryTag);
    if (!poster.isEmpty() && m_images) {
        QNetworkReply *reply = m_images->get(QNetworkRequest(poster));
        const QString serverId = record.serverId;
        const QString userId = record.userId;
        const QString itemId = record.itemId;
        connect(reply, &QNetworkReply::finished, this, [this, reply, serverId, userId, itemId] {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError)
                return;
            const QByteArray data = reply->readAll();
            if (data.isEmpty() || data.size() > kMaxPosterBytes)
                return;
            // The download may have been removed meanwhile: write into the folder it still has, never create one
            const std::optional<DownloadRecord> current =
                m_manager.store()->find(serverId, userId, itemId);
            const QString media = current ? m_manager.localPath(*current) : QString();
            if (media.isEmpty())
                return;
            const QString directory = QFileInfo(media).absolutePath();
            if (!QFileInfo(directory).isDir())
                return;
            if (writePrivateFile(directory + QStringLiteral("/poster.jpg"), data)) {
                ++m_revision;
                Q_EMIT revisionChanged();
            }
        });
    }
}

int DownloadsController::pendingSyncCount() const
{
    int count = 0;
    for (const WatchedEvent &event : m_outbox.events()) {
        if (ownedByCurrent(event.serverId, event.userId))
            ++count;
    }
    return count;
}

QVariantList DownloadsController::outboxConflicts() const
{
    QVariantList conflicts;
    for (const WatchedEvent &event : m_outbox.events()) {
        if (!event.conflict || !ownedByCurrent(event.serverId, event.userId))
            continue;
        QString name = event.itemId;
        if (const std::optional<DownloadRecord> record =
                m_manager.store()->find(event.serverId, event.userId, event.itemId))
            name = record->name;
        conflicts.append(QVariantMap{{QStringLiteral("serverId"), event.serverId},
                                     {QStringLiteral("userId"), event.userId},
                                     {QStringLiteral("itemId"), event.itemId},
                                     {QStringLiteral("name"), name}});
    }
    return conflicts;
}

bool DownloadsController::resolveOutboxConflict(const QString &serverId, const QString &userId,
                                                const QString &itemId, bool keepServer)
{
    const std::optional<WatchedEvent> found = m_outbox.find(serverId, userId, itemId);
    if (!found || !found->conflict || !ownedByCurrent(serverId, userId))
        return false;
    if (keepServer) {
        const bool removed = m_outbox.remove(serverId, userId, itemId);
        Q_EMIT outboxChanged();
        return removed;
    }
    // Only the signed-in owner can tell the server
    if (!m_client->isAuthenticated()
        || serverId.compare(m_client->serverId(), Qt::CaseInsensitive) != 0
        || userId.compare(m_client->userId(), Qt::CaseInsensitive) != 0)
        return false;
    WatchedEvent event = *found;
    event.conflict = false;
    // The user's choice is newer than the server's play: a refused write is not flagged again
    event.createdAt = QDateTime::currentMSecsSinceEpoch();
    if (!m_outbox.append(event))
        return false;
    m_outboxWrites.insert(event.itemId);
    if (!m_client->setItemPlayed(event.itemId, true))
        m_outboxWrites.remove(event.itemId);
    Q_EMIT outboxChanged();
    return true;
}

bool DownloadsController::setSeriesRule(const QString &seriesId, const QString &seriesName,
                                        const QString &kind, int seasonNumber, int count)
{
    if (m_client->userId().isEmpty() || !isValidJellyfinId(seriesId)
        || !QStringList{QStringLiteral("nextN"), QStringLiteral("season"),
                        QStringLiteral("newEpisodes")}
                .contains(kind)
        || (kind == QLatin1String("season") && seasonNumber < 1))
        return false;
    DownloadRule rule;
    rule.serverId = m_client->serverId();
    rule.userId = m_client->userId();
    rule.seriesId = seriesId;
    rule.seriesName = cleanText(seriesName, DownloadRecord::MaxName);
    rule.kind = kind;
    rule.seasonNumber = seasonNumber;
    rule.count = std::clamp(count, 1, 3);
    rule.enabled = true;
    const std::optional<DownloadRule> existing =
        m_rules.find(rule.serverId, rule.userId, rule.seriesId);
    if (existing) {
        rule.cursorDate = existing->cursorDate;
        rule.cursorId = existing->cursorId;
    }
    if (kind == QLatin1String("newEpisodes") && rule.cursorDate.isEmpty()) {
        // The rule watches what is added from now on, not the whole back catalogue
        rule.cursorDate = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    }
    if (!m_rules.upsert(rule))
        return false;
    scanRules();
    return true;
}

bool DownloadsController::removeSeriesRule(const QString &seriesId)
{
    if (m_client->userId().isEmpty())
        return false;
    return m_rules.remove(m_client->serverId(), m_client->userId(), seriesId);
}

QVariantList DownloadsController::seriesRules() const
{
    QVariantList result;
    if (m_client->userId().isEmpty())
        return result;
    for (const DownloadRule &rule : m_rules.rules()) {
        if (rule.serverId.compare(m_client->serverId(), Qt::CaseInsensitive) != 0
            || rule.userId.compare(m_client->userId(), Qt::CaseInsensitive) != 0)
            continue;
        result.append(QVariantMap{{QStringLiteral("seriesId"), rule.seriesId},
                                  {QStringLiteral("seriesName"), rule.seriesName},
                                  {QStringLiteral("kind"), rule.kind},
                                  {QStringLiteral("seasonNumber"), rule.seasonNumber},
                                  {QStringLiteral("count"), rule.count}});
    }
    return result;
}

QVariantMap DownloadsController::seriesRule(const QString &seriesId) const
{
    if (m_client->userId().isEmpty())
        return {};
    const std::optional<DownloadRule> rule =
        m_rules.find(m_client->serverId(), m_client->userId(), seriesId);
    if (!rule)
        return {};
    return {{QStringLiteral("kind"), rule->kind},
            {QStringLiteral("seasonNumber"), rule->seasonNumber},
            {QStringLiteral("count"), rule->count},
            {QStringLiteral("enabled"), rule->enabled}};
}

void DownloadsController::scheduleRuleScan()
{
    m_ruleDebounce.start();
}

void DownloadsController::scanRules()
{
    if (m_client->userId().isEmpty())
        return;
    for (const DownloadRule &rule : m_rules.rules()) {
        if (!rule.enabled || rule.serverId.compare(m_client->serverId(), Qt::CaseInsensitive) != 0
            || rule.userId.compare(m_client->userId(), Qt::CaseInsensitive) != 0)
            continue;
        bool pending = false;
        for (const DownloadRule &fetch : std::as_const(m_ruleFetches)) {
            if (fetch.key() == rule.key()) {
                pending = true;
                break;
            }
        }
        if (pending)
            continue;
        const int channel = m_nextChannel++;
        if (!m_client->fetchEpisodes(rule.seriesId, {}, 0, 200, channel, true))
            continue;
        m_ruleFetches.insert(channel, rule);
    }
}

void DownloadsController::onEpisodesReady(const QJsonArray &items, int totalRecordCount,
                                          int channel)
{
    const auto fetch = m_ruleFetches.find(channel);
    if (fetch == m_ruleFetches.end())
        return;
    const DownloadRule started = fetch.value();
    m_ruleFetches.erase(fetch);
    m_client->releaseListingChannel(channel);
    QList<QJsonObject> accumulated = m_ruleItems.value(started.key());
    for (const QJsonValue &value : items) {
        if (value.isObject())
            accumulated.append(value.toObject());
    }
    // A long series is read page by page; the limit stays bounded
    if (items.size() == 200 && accumulated.size() < totalRecordCount) {
        const int nextChannel = m_nextChannel++;
        if (m_client->fetchEpisodes(started.seriesId, {}, int(accumulated.size()), 200, nextChannel,
                                    true)) {
            m_ruleItems.insert(started.key(), accumulated);
            m_ruleFetches.insert(nextChannel, started);
            return;
        }
        // A cursor never moves over pages that were not read: the next scan starts over
        m_ruleItems.remove(started.key());
        return;
    }
    m_ruleItems.remove(started.key());

    // The rule may have been stopped or edited while the listing ran: the stored one decides
    const std::optional<DownloadRule> current =
        m_rules.find(started.serverId, started.userId, started.seriesId);
    if (!current || !current->enabled
        || current->serverId.compare(m_client->serverId(), Qt::CaseInsensitive) != 0
        || current->userId.compare(m_client->userId(), Qt::CaseInsensitive) != 0)
        return;
    if (current->kind != started.kind) {
        scanRules();
        return;
    }
    DownloadRule rule = *current;

    struct Episode {
        QString id;
        int season = -1;
        int number = -1;
        QDateTime created;
    };
    const bool watchesNew = rule.kind == QLatin1String("newEpisodes");
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QDateTime cursor = parseServerDate(rule.cursorDate);
    if (watchesNew && !cursor.isValid()) {
        // A rule without a starting point watches from now on
        rule.cursorDate = now.toString(Qt::ISODateWithMs);
        m_rules.upsert(rule);
        return;
    }
    QList<Episode> episodes;
    for (const QJsonObject &object : std::as_const(accumulated)) {
        if (object.value(QStringLiteral("Type")).toString() != QLatin1String("Episode"))
            continue;
        const QString id = object.value(QStringLiteral("Id")).toString();
        if (!isValidJellyfinId(id))
            continue;
        // A missing or unaired episode has no file to download
        if (object.value(QStringLiteral("LocationType")).toString() == QLatin1String("Virtual"))
            continue;
        if (object.value(QStringLiteral("UserData"))
                .toObject()
                .value(QStringLiteral("Played"))
                .toBool(false))
            continue;
        const QJsonValue premiere = object.value(QStringLiteral("PremiereDate"));
        if (premiere.isString()) {
            const QDateTime date = parseServerDate(premiere.toString());
            if (date.isValid() && date > now)
                continue;
        }
        Episode episode;
        episode.id = id;
        episode.season = object.value(QStringLiteral("ParentIndexNumber")).toInt(-1);
        episode.number = object.value(QStringLiteral("IndexNumber")).toInt(-1);
        episode.created = parseServerDate(object.value(QStringLiteral("DateCreated")).toString());
        // Without a creation date the cursor cannot tell whether the episode is new
        if (watchesNew && (!episode.created.isValid() || episode.created <= cursor))
            continue;
        episodes.append(episode);
    }
    std::sort(episodes.begin(), episodes.end(),
              [watchesNew](const Episode &left, const Episode &right) {
                  // New episodes are taken oldest first, so the cursor only moves forward
                  if (watchesNew && left.created != right.created)
                      return left.created < right.created;
                  if (left.season != right.season)
                      return left.season < right.season;
                  if (left.number != right.number)
                      return left.number < right.number;
                  return left.created < right.created;
              });
    if (rule.kind == QLatin1String("season"))
        episodes.removeIf(
            [&rule](const Episode &episode) { return episode.season != rule.seasonNumber; });
    if (rule.kind == QLatin1String("nextN") && episodes.size() > rule.count)
        episodes = episodes.mid(0, rule.count);
    if (episodes.size() > 50)
        episodes = episodes.mid(0, 50);
    QDateTime newest;
    QString newestId;
    QDateTime refusedAt;
    bool refused = false;
    for (const Episode &episode : std::as_const(episodes)) {
        // An episode that already has a record is handled; a refusal is retried by the next scan
        const bool handled =
            m_manager.store()->find(rule.serverId, rule.userId, episode.id).has_value()
            || downloadItemForRule(episode.id, rule.seriesId, episode.created);
        if (!handled) {
            if (!refused)
                refusedAt = episode.created;
            refused = true;
            continue;
        }
        // The cursor stops at the first refusal: everything after it is offered again
        if (!refused && episode.created.isValid()
            && (!newest.isValid() || episode.created > newest)) {
            newest = episode.created;
            newestId = episode.id;
        }
    }
    // An accepted episode created no earlier than the refused one would hide it from the next scan
    if (refused && newest.isValid() && (!refusedAt.isValid() || newest >= refusedAt))
        newest = QDateTime();
    if (newest.isValid() && (!cursor.isValid() || newest > cursor)) {
        rule.cursorDate = newest.toUTC().toString(Qt::ISODateWithMs);
        rule.cursorId = newestId;
        m_rules.upsert(rule);
    }
}

void DownloadsController::exportDownload(const QString &serverId, const QString &userId,
                                         const QString &itemId, const QUrl &folder)
{
    const std::optional<DownloadRecord> found = m_manager.store()->find(serverId, userId, itemId);
    const QString media = found ? m_manager.localPath(*found) : QString();
    const QString root = folder.toLocalFile();
    const QFileInfo folderInfo(root);
    // The copy runs off the UI thread, one at a time
    if (m_exportThread) {
        Q_EMIT exportBusy(itemId);
        return;
    }
    if (media.isEmpty() || root.isEmpty() || !folderInfo.isAbsolute() || !folderInfo.isDir()) {
        Q_EMIT exportFinished(itemId, false);
        return;
    }
    const QFileInfo relative(ExportNames::relativePath(*found));
    const QString directory = QDir(root).filePath(relative.path());
    const bool sidecars = m_kodiSidecars;
    const QString poster = QFileInfo(media).absolutePath() + QStringLiteral("/poster.jpg");
    const bool withPoster = sidecars && QFileInfo::exists(poster);
    const QString base = freeExportBase(directory, relative.completeBaseName(), relative.suffix(),
                                        sidecars, withPoster);
    if (base.isEmpty() || !QDir().mkpath(directory)) {
        Q_EMIT exportFinished(itemId, false);
        return;
    }
    const DownloadRecord record = *found;
    const QString stem = directory + QLatin1Char('/') + base;
    const QString target = stem + QLatin1Char('.') + relative.suffix();
    QThread *thread =
        QThread::create([this, record, media, target, stem, poster, withPoster, sidecars] {
            // Every file is created new: nothing that was already there is truncated or replaced
            const QString part = target + QStringLiteral(".part");
            const bool copied = copyNewFile(media, part);
            const bool ok = copied && renameNoReplace(part, target);
            if (copied && !ok)
                QFile::remove(part);
            if (ok && sidecars) {
                const bool episode = record.type == QLatin1String("Episode");
                QString xml = QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
                if (episode) {
                    xml += QStringLiteral("<episodedetails><title>%1</title><season>%2</season>"
                                          "<episode>%3</episode></episodedetails>\n")
                               .arg(record.name.toHtmlEscaped())
                               .arg(record.seasonNumber)
                               .arg(record.episodeNumber);
                } else {
                    xml += QStringLiteral("<movie><title>%1</title><year>%2</year></movie>\n")
                               .arg(record.name.toHtmlEscaped())
                               .arg(record.year);
                }
                QFile nfo(stem + QStringLiteral(".nfo"));
                if (nfo.open(QIODevice::WriteOnly | QIODevice::NewOnly))
                    nfo.write(xml.toUtf8());
                if (withPoster)
                    copyNewFile(poster, stem + QStringLiteral("-poster.jpg"));
            }
            QMetaObject::invokeMethod(
                this,
                [this, record, ok] {
                    if (ok)
                        m_manager.setState(record.serverId, record.userId, record.itemId,
                                           QStringLiteral("exported"));
                    Q_EMIT exportFinished(record.itemId, ok);
                },
                Qt::QueuedConnection);
        });
    m_exportThread = thread;
    connect(thread, &QThread::finished, this, [this, thread] {
        m_exportThread = nullptr;
        thread->deleteLater();
    });
    thread->start();
}

QUrl DownloadsController::posterUrl(const QString &itemId, const QString &tag) const
{
    if (tag.isEmpty() || !isValidJellyfinId(itemId))
        return {};
    return m_client->imageUrl(itemId, QStringLiteral("Primary"), 160, tag);
}

QUrl DownloadsController::posterSource(const QString &serverId, const QString &userId,
                                       const QString &itemId, const QString &tag) const
{
    const std::optional<DownloadRecord> record = m_manager.store()->find(serverId, userId, itemId);
    if (record) {
        const QString media = m_manager.localPath(*record);
        if (!media.isEmpty()) {
            const QString poster = QFileInfo(media).absolutePath() + QStringLiteral("/poster.jpg");
            if (QFileInfo::exists(poster))
                return QUrl::fromLocalFile(poster);
        }
    }
    return posterUrl(itemId, tag);
}

QVariantMap DownloadsController::playableDownload(const QString &serverId, const QString &userId,
                                                  const QString &itemId) const
{
    const std::optional<DownloadRecord> record = m_manager.store()->find(serverId, userId, itemId);
    if (!record)
        return {};
    const QString path = m_manager.localPath(*record);
    if (path.isEmpty())
        return {};
    return {{QStringLiteral("path"), path},
            {QStringLiteral("name"), record->name},
            {QStringLiteral("type"), record->type},
            {QStringLiteral("seriesName"), record->seriesName},
            {QStringLiteral("seasonNumber"), record->seasonNumber},
            {QStringLiteral("episodeNumber"), record->episodeNumber},
            {QStringLiteral("positionTicks"), record->positionTicks}};
}

QString DownloadsController::formattedSize(qint64 bytes) const
{
    if (bytes < 0)
        return {};
    return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
}

void DownloadsController::reconcileOutbox()
{
    if (m_client->userId().isEmpty())
        return;
    reconcileNext();
}

void DownloadsController::reconcileNext()
{
    // One event at a time: a read, a write or its verification still in flight holds the next one
    if (!m_outboxFetches.isEmpty() || !m_outboxVerifies.isEmpty() || !m_outboxWrites.isEmpty())
        return;
    for (const WatchedEvent &event : m_outbox.events()) {
        // A conflict waits for the user's choice and must not hold the events behind it
        if (event.conflict || event.serverId.compare(m_client->serverId(), Qt::CaseInsensitive) != 0
            || event.userId.compare(m_client->userId(), Qt::CaseInsensitive) != 0)
            continue;
        ItemQuery query;
        query.itemIds = {event.itemId};
        const int channel = m_nextChannel++;
        if (!m_client->fetchItems(query, channel))
            return;
        m_outboxFetches.insert(channel, event.key());
        return;
    }
}

void DownloadsController::onOutboxItemsReady(const QJsonArray &items, int channel)
{
    const auto fetch = m_outboxFetches.find(channel);
    if (fetch == m_outboxFetches.end())
        return;
    const QString key = fetch.value();
    m_outboxFetches.erase(fetch);
    m_client->releaseListingChannel(channel);
    std::optional<WatchedEvent> event;
    for (const WatchedEvent &candidate : m_outbox.events()) {
        if (candidate.key() == key) {
            event = candidate;
            break;
        }
    }
    if (!event) {
        reconcileNext();
        return;
    }
    if (items.isEmpty()) {
        // The item is gone: nothing left to tell the server
        m_outbox.remove(event->serverId, event->userId, event->itemId);
        Q_EMIT outboxChanged();
        reconcileNext();
        return;
    }
    const QJsonObject userData =
        items.first().toObject().value(QStringLiteral("UserData")).toObject();
    if (userData.value(QStringLiteral("Played")).toBool(false)) {
        m_outbox.remove(event->serverId, event->userId, event->itemId);
        Q_EMIT outboxChanged();
        reconcileNext();
        return;
    }
    // A newer play on the server is a conflict: surface it, never overwrite it silently
    const QDateTime serverPlayed = QDateTime::fromString(
        userData.value(QStringLiteral("LastPlayedDate")).toString(), Qt::ISODate);
    if (serverPlayed.isValid() && serverPlayed.toMSecsSinceEpoch() > event->createdAt) {
        event->conflict = true;
        m_outbox.append(*event);
        Q_EMIT outboxChanged();
        reconcileNext();
        return;
    }
    m_outboxWrites.insert(event->itemId);
    if (!m_client->setItemPlayed(event->itemId, true))
        m_outboxWrites.remove(event->itemId);
}

void DownloadsController::onItemsReady(const QJsonArray &items, int totalRecordCount, int channel)
{
    Q_UNUSED(totalRecordCount)
    if (m_outboxVerifies.contains(channel)) {
        const QString key = m_outboxVerifies.take(channel);
        m_client->releaseListingChannel(channel);
        bool played = false;
        if (!items.isEmpty()) {
            played = items.first()
                         .toObject()
                         .value(QStringLiteral("UserData"))
                         .toObject()
                         .value(QStringLiteral("Played"))
                         .toBool(false);
        }
        for (const WatchedEvent &event : m_outbox.events()) {
            if (event.key() != key)
                continue;
            if (played) {
                m_outbox.remove(event.serverId, event.userId, event.itemId);
            } else {
                // The server accepted the write but still reports the item unplayed: do not loop on it
                qCWarning(downloadsLog) << "The watched state was not kept by the server";
                m_outbox.remove(event.serverId, event.userId, event.itemId);
            }
            break;
        }
        Q_EMIT outboxChanged();
        reconcileNext();
        return;
    }
    if (m_outboxFetches.contains(channel)) {
        onOutboxItemsReady(items, channel);
        return;
    }
    const auto fetch = m_fetches.find(channel);
    if (fetch == m_fetches.end())
        return;
    const QString itemId = fetch.value();
    m_fetches.erase(fetch);
    m_fetching.remove(itemId);
    m_client->releaseListingChannel(channel);
    const QString ruleId = m_fetchRules.take(channel);
    const QDateTime created = m_fetchRuleDates.take(channel);
    // A background scan never toasts: only the user's own download reports back
    const auto fail = [this, &itemId, &ruleId, &created](const QString &reason) {
        if (ruleId.isEmpty()) {
            Q_EMIT downloadFailed(itemId, reason);
            return;
        }
        qCInfo(downloadsLog) << "A rule download was not queued:" << reason;
        rewindRuleCursor(ruleId, created);
    };
    // Only the item that was asked for: a reply naming another one is not a download of it
    if (items.isEmpty()
        || items.first()
                   .toObject()
                   .value(QStringLiteral("Id"))
                   .toString()
                   .compare(itemId, Qt::CaseInsensitive)
               != 0) {
        fail(QStringLiteral("notfound"));
        return;
    }
    const std::optional<DownloadRecord> record = recordFor(items.first().toObject(), ruleId);
    if (!record) {
        fail(QStringLiteral("unsupported"));
        return;
    }
    if (!m_manager.enqueue(*record)) {
        fail(QStringLiteral("queue"));
        return;
    }
    if (ruleId.isEmpty())
        Q_EMIT downloadEnqueued(itemId);
}

void DownloadsController::onListingFailed(const QString &family, int httpStatus,
                                          const QString &reason, int channel)
{
    Q_UNUSED(reason)
    if (m_outboxVerifies.remove(channel) > 0) {
        // Keep the event for the next trigger instead of looping on a failing read
        m_client->releaseListingChannel(channel);
        return;
    }
    if (m_outboxFetches.contains(channel)) {
        m_outboxFetches.remove(channel);
        m_client->releaseListingChannel(channel);
        return;
    }
    if (m_ruleFetches.contains(channel)) {
        // The rule stays; the next scan retries the listing from the first page
        m_ruleItems.remove(m_ruleFetches.take(channel).key());
        m_client->releaseListingChannel(channel);
        return;
    }
    const auto fetch = m_fetches.find(channel);
    if (fetch == m_fetches.end() || family != QLatin1String("Items"))
        return;
    const QString itemId = fetch.value();
    m_fetches.erase(fetch);
    m_fetching.remove(itemId);
    m_client->releaseListingChannel(channel);
    // A background scan never toasts
    const QString ruleId = m_fetchRules.take(channel);
    const QDateTime created = m_fetchRuleDates.take(channel);
    if (ruleId.isEmpty())
        Q_EMIT downloadFailed(itemId, failureForStatus(httpStatus));
    else
        rewindRuleCursor(ruleId, created);
}

std::optional<DownloadRecord> DownloadsController::recordFor(const QJsonObject &item,
                                                             const QString &ruleId) const
{
    const ItemDetails details = parseItemDetails(item);
    if (!details.valid || !details.canDownload || details.name.isEmpty()
        || !kDownloadableTypes.contains(details.type))
        return std::nullopt;
    const QString serverId = m_client->serverId();
    const QString userId = m_client->userId();
    if (serverId.isEmpty() || userId.isEmpty())
        return std::nullopt;

    DownloadRecord record;
    record.serverId = serverId;
    record.userId = userId;
    record.itemId = details.id;
    record.name = cleanText(details.name, DownloadRecord::MaxName);
    if (record.name.isEmpty())
        return std::nullopt;
    record.type = details.type;
    record.ruleId = ruleId;
    record.seriesName = cleanText(details.seriesName, DownloadRecord::MaxName);
    record.seriesId = details.seriesId;
    record.seasonId = details.seasonId;
    record.seasonNumber = std::min(details.seasonNumber, DownloadRecord::MaxIndexNumber);
    record.episodeNumber = std::min(details.episodeNumber, DownloadRecord::MaxIndexNumber);
    record.year = details.year;
    // A manual start on a metered network is the user's explicit choice; a rule waits for the meter
    record.meteredOverride = ruleId.isEmpty() && m_meter.metered();
    if (details.runtimeMinutes > 0) {
        record.runtimeTicks = std::min(qint64(details.runtimeMinutes) * 60 * 10'000'000,
                                       DownloadRecord::MaxRuntimeTicks);
    }
    if (!details.sources.isEmpty()) {
        const SourceSummary &source = details.sources.constFirst();
        record.container = source.container;
        record.size = source.size > 0 ? source.size : -1;
        record.etag = cleanText(source.etag, DownloadRecord::MaxEtag);
    }
    record.primaryTag = cleanText(details.primaryTag, DownloadRecord::MaxTag);
    return record;
}
} // namespace Kinefin
