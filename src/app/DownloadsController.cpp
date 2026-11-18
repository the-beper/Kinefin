// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "DownloadsController.h"

#include "api/MediaSourceSelection.h"
#include "downloads/ExportNames.h"
#include "models/ItemDetails.h"
#include "settings/ProfilePaths.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QThread>
#include <QTimer>
#include <QUuid>

#include <algorithm>

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
                    if (event.itemId.compare(itemId, Qt::CaseInsensitive) != 0)
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
    connect(m_client, &JellyfinClient::loginSucceeded, this, [this] { refreshCanDownload(); });
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
        m_fetching.clear();
        m_outboxFetches.clear();
        m_outboxVerifies.clear();
        m_outboxWrites.clear();
        m_ruleFetches.clear();
        m_ruleItems.clear();
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

bool DownloadsController::downloadItemForRule(const QString &itemId, const QString &ruleId)
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
    m_fetching.insert(itemId.toLower());
    return true;
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

void DownloadsController::updateLocalProgress(const QString &itemId, qint64 ticks, bool played)
{
    if (ticks < 0 || ticks > DownloadRecord::MaxRuntimeTicks)
        return;
    for (DownloadRecord record : m_manager.store()->records()) {
        if (record.itemId.compare(itemId, Qt::CaseInsensitive) != 0)
            continue;
        if (record.state != QLatin1String("completed") && record.state != QLatin1String("exported"))
            return;
        record.positionTicks = ticks;
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
        m_manager.store()->upsert(record, played);
        return;
    }
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
    if (!record.primaryTag.isEmpty()) {
        QNetworkRequest request(posterUrl(record.itemId, record.primaryTag));
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::ManualRedirectPolicy);
        QNetworkReply *reply = m_imageNetwork.get(request);
        connect(reply, &QNetworkReply::finished, this, [this, reply, directory] {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError)
                return;
            const QByteArray data = reply->readAll();
            if (data.isEmpty() || data.size() > 2 * 1024 * 1024)
                return;
            if (writePrivateFile(directory + QStringLiteral("/poster.jpg"), data)) {
                ++m_revision;
                Q_EMIT revisionChanged();
            }
        });
    }
}

QVariantList DownloadsController::outboxConflicts() const
{
    QVariantList conflicts;
    for (const WatchedEvent &event : m_outbox.events()) {
        if (!event.conflict)
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
    if (!found || !found->conflict)
        return false;
    if (keepServer) {
        const bool removed = m_outbox.remove(serverId, userId, itemId);
        Q_EMIT outboxChanged();
        return removed;
    }
    WatchedEvent event = *found;
    event.conflict = false;
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
    } else if (kind == QLatin1String("newEpisodes")) {
        // The rule watches what airs from now on, not the whole back catalogue
        rule.cursorDate = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
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
        if (!m_client->fetchEpisodes(rule.seriesId, {}, 0, 200, channel))
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
    DownloadRule rule = fetch.value();
    m_ruleFetches.erase(fetch);
    m_client->releaseListingChannel(channel);
    QList<QJsonObject> accumulated = m_ruleItems.value(rule.key());
    for (const QJsonValue &value : items) {
        if (value.isObject())
            accumulated.append(value.toObject());
    }
    // A long series is read page by page; the limit stays bounded
    if (items.size() == 200 && accumulated.size() < totalRecordCount) {
        const int nextChannel = m_nextChannel++;
        if (m_client->fetchEpisodes(rule.seriesId, {}, int(accumulated.size()), 200, nextChannel)) {
            m_ruleItems.insert(rule.key(), accumulated);
            m_ruleFetches.insert(nextChannel, rule);
            return;
        }
    }
    m_ruleItems.remove(rule.key());

    struct Episode {
        QString id;
        int season = -1;
        int number = -1;
        QString created;
    };
    QList<Episode> episodes;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (const QJsonObject &object : std::as_const(accumulated)) {
        if (object.value(QStringLiteral("Type")).toString() != QLatin1String("Episode"))
            continue;
        const QString id = object.value(QStringLiteral("Id")).toString();
        if (!isValidJellyfinId(id))
            continue;
        if (object.value(QStringLiteral("UserData"))
                .toObject()
                .value(QStringLiteral("Played"))
                .toBool(false))
            continue;
        const QJsonValue premiere = object.value(QStringLiteral("PremiereDate"));
        if (premiere.isString()) {
            const QDateTime date = QDateTime::fromString(premiere.toString(), Qt::ISODate);
            if (date.isValid() && date > now)
                continue;
        }
        Episode episode;
        episode.id = id;
        episode.season = object.value(QStringLiteral("ParentIndexNumber")).toInt(-1);
        episode.number = object.value(QStringLiteral("IndexNumber")).toInt(-1);
        episode.created = object.value(QStringLiteral("DateCreated")).toString();
        episodes.append(episode);
    }
    std::sort(episodes.begin(), episodes.end(), [](const Episode &left, const Episode &right) {
        if (left.season != right.season)
            return left.season < right.season;
        if (left.number != right.number)
            return left.number < right.number;
        return left.created < right.created;
    });
    if (rule.kind == QLatin1String("season"))
        episodes.removeIf(
            [&rule](const Episode &episode) { return episode.season != rule.seasonNumber; });
    if (rule.kind == QLatin1String("newEpisodes") && !rule.cursorDate.isEmpty())
        episodes.removeIf(
            [&rule](const Episode &episode) { return episode.created <= rule.cursorDate; });
    if (rule.kind == QLatin1String("nextN") && episodes.size() > rule.count)
        episodes = episodes.mid(0, rule.count);
    if (episodes.size() > 50)
        episodes = episodes.mid(0, 50);
    QString newestDate = rule.cursorDate;
    QString newestId = rule.cursorId;
    for (const Episode &episode : std::as_const(episodes)) {
        // Only an accepted item moves the cursor: a transient failure is retried by the next scan
        if (downloadItemForRule(episode.id, rule.seriesId) && episode.created > newestDate) {
            newestDate = episode.created;
            newestId = episode.id;
        }
    }
    if (newestDate != rule.cursorDate || newestId != rule.cursorId) {
        rule.cursorDate = newestDate;
        rule.cursorId = newestId;
        m_rules.upsert(rule);
    }
}

void DownloadsController::exportDownload(const QString &serverId, const QString &userId,
                                         const QString &itemId, const QUrl &folder)
{
    const std::optional<DownloadRecord> found = m_manager.store()->find(serverId, userId, itemId);
    if (!found
        || (found->state != QLatin1String("completed")
            && found->state != QLatin1String("exported")))
        return;
    const QString media = m_manager.localPath(*found);
    const QString root = folder.toLocalFile();
    const QFileInfo folderInfo(root);
    if (media.isEmpty() || root.isEmpty() || !folderInfo.isAbsolute() || !folderInfo.isDir())
        return;
    const QString target = ExportNames::availablePath(root, *found);
    if (!QDir().mkpath(QFileInfo(target).absolutePath()))
        return;
    const DownloadRecord record = *found;
    const QString itemDirectory = QFileInfo(media).absolutePath();
    const bool sidecars = m_kodiSidecars;
    // The copy runs off the UI thread; the record and the sidecars are settled when it ends
    if (m_exportThread)
        return;
    QThread *thread = QThread::create([this, record, media, target, itemDirectory, sidecars] {
        const QString part = target + QStringLiteral(".part");
        QFile::remove(part);
        bool ok = QFile::copy(media, part) && QFile::rename(part, target);
        if (!ok)
            QFile::remove(part);
        if (ok && sidecars) {
            const QFileInfo targetInfo(target);
            const QString base =
                targetInfo.absolutePath() + QLatin1Char('/') + targetInfo.completeBaseName();
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
            QFile nfo(base + QStringLiteral(".nfo"));
            if (nfo.open(QIODevice::WriteOnly | QIODevice::Truncate))
                nfo.write(xml.toUtf8());
            const QString poster = itemDirectory + QStringLiteral("/poster.jpg");
            if (QFileInfo::exists(poster))
                QFile::copy(poster, base + QStringLiteral("-poster.jpg"));
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
    if (!m_outboxFetches.isEmpty() || !m_outboxWrites.isEmpty())
        return;
    for (const WatchedEvent &event : m_outbox.events()) {
        if (event.serverId.compare(m_client->serverId(), Qt::CaseInsensitive) != 0
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
    if (event->conflict) {
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
    if (items.isEmpty()) {
        Q_EMIT downloadFailed(itemId, QStringLiteral("notfound"));
        return;
    }
    const QString ruleId = m_fetchRules.take(channel);
    const std::optional<DownloadRecord> record = recordFor(items.first().toObject(), ruleId);
    if (!record) {
        Q_EMIT downloadFailed(itemId, QStringLiteral("unsupported"));
        return;
    }
    if (!m_manager.enqueue(*record)) {
        Q_EMIT downloadFailed(itemId, QStringLiteral("queue"));
        return;
    }
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
        // The rule stays; the next scan retries the listing
        m_ruleFetches.remove(channel);
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
    Q_EMIT downloadFailed(itemId, failureForStatus(httpStatus));
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
