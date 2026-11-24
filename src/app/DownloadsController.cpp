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
#include <QSslError>
#include <QThread>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <fcntl.h>
#include <utility>

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

// The worst valid info file (200 chapters of 256 characters) is about 0.25 MiB
constexpr qint64 kMaxInfoBytes = 512 * 1024;
constexpr int kMaxSubtitleTitle = 120;

// One line of server text: control, format (bidirectional overrides among them) and line-separator characters become spaces, then whitespace runs collapse
QString oneLineText(const QString &text, int max)
{
    QString cleaned = text;
    for (QChar &character : cleaned) {
        const QChar::Category category = character.category();
        if (category == QChar::Other_Control || category == QChar::Other_Format
            || category == QChar::Separator_Line || category == QChar::Separator_Paragraph)
            character = QLatin1Char(' ');
    }
    return cleaned.simplified().left(max);
}

QString validLanguage(const QString &language)
{
    static const QRegularExpression pattern(QStringLiteral("^[a-z]{2,3}\\z"));
    return pattern.match(language).hasMatch() ? language : QString();
}

// The file extension of a text subtitle codec the controller keeps, empty for any other; in step with JellyfinClient::subtitleFileUrl and localSubtitles in PlayerController.cpp
QString subtitleExtension(const QString &codec)
{
    if (codec == QLatin1String("srt") || codec == QLatin1String("subrip"))
        return QStringLiteral("srt");
    if (codec == QLatin1String("vtt") || codec == QLatin1String("webvtt"))
        return QStringLiteral("vtt");
    if (codec == QLatin1String("ass") || codec == QLatin1String("ssa"))
        return codec;
    return {};
}

// An external text subtitle stream of the server's MediaStreams, nothing else
std::optional<SubtitleTrack> subtitleTrackOf(const QJsonObject &stream)
{
    if (stream.value(QStringLiteral("Type")).toString() != QLatin1String("Subtitle")
        || !stream.value(QStringLiteral("IsExternal")).toBool(false)
        || !stream.value(QStringLiteral("IsTextSubtitleStream")).toBool(false))
        return std::nullopt;
    const QString extension =
        subtitleExtension(stream.value(QStringLiteral("Codec")).toString().toLower());
    const QJsonValue index = stream.value(QStringLiteral("Index"));
    if (extension.isEmpty() || !index.isDouble())
        return std::nullopt;
    const double number = index.toDouble();
    if (!(number >= 0 && number <= 9999) || number != std::floor(number))
        return std::nullopt;
    SubtitleTrack track;
    track.index = int(number);
    track.extension = extension;
    track.language = validLanguage(stream.value(QStringLiteral("Language")).toString());
    track.title =
        oneLineText(stream.value(QStringLiteral("DisplayTitle")).toString(), kMaxSubtitleTitle);
    if (track.title.isEmpty())
        track.title =
            oneLineText(stream.value(QStringLiteral("Title")).toString(), kMaxSubtitleTitle);
    return track;
}

QJsonObject infoObject(const DownloadRecord &record)
{
    return {{QStringLiteral("name"), record.name},
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
}

// The chapters and subtitle files the info file of `directory` lists, validated again: the file is only as trustworthy as the disk, so a missing or malformed one gives empty lists
void readInfoExtras(const QString &directory, QVariantList *chapters, QVariantList *subtitles)
{
    const QFileInfo infoFile(directory + QStringLiteral("/info.json"));
    if (!infoFile.isFile() || infoFile.isSymLink())
        return;
    QFile file(infoFile.filePath());
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QByteArray bytes = file.read(kMaxInfoBytes + 1);
    if (bytes.size() > kMaxInfoBytes)
        return;
    const QJsonDocument document = QJsonDocument::fromJson(bytes);
    if (!document.isObject())
        return;
    const QJsonObject info = document.object();
    for (const ChapterSummary &chapter : parseChapters(info.value(QStringLiteral("chapters")))) {
        if (chapter.startTicks > DownloadRecord::MaxRuntimeTicks)
            continue;
        chapters->append(QVariantMap{{QStringLiteral("name"), chapter.name},
                                     {QStringLiteral("startTicks"), chapter.startTicks}});
    }
    const QFileInfo subsDirectory(directory + QStringLiteral("/subs"));
    if (!subsDirectory.isDir() || subsDirectory.isSymLink())
        return;
    const QString canonicalSubs = subsDirectory.canonicalFilePath();
    static const QRegularExpression fileName(
        QStringLiteral("^subs/([0-9]{1,4})\\.(srt|ass|ssa|vtt)\\z"));
    for (const QJsonValue &value : info.value(QStringLiteral("subtitles")).toArray()) {
        if (subtitles->size() == DownloadsController::MaxSubtitleTracks)
            break;
        const QJsonObject entry = value.toObject();
        const QRegularExpressionMatch match =
            fileName.match(entry.value(QStringLiteral("file")).toString());
        // The index is written without leading zeros
        if (!match.hasMatch() || QString::number(match.captured(1).toInt()) != match.captured(1))
            continue;
        const QFileInfo subtitle(directory + QLatin1Char('/') + match.captured(0));
        if (!subtitle.isFile() || subtitle.isSymLink()
            || !subtitle.canonicalFilePath().startsWith(canonicalSubs + QLatin1Char('/')))
            continue;
        subtitles->append(QVariantMap{
            {QStringLiteral("path"), subtitle.absoluteFilePath()},
            {QStringLiteral("language"),
             validLanguage(entry.value(QStringLiteral("language")).toString())},
            {QStringLiteral("title"),
             oneLineText(entry.value(QStringLiteral("title")).toString(), kMaxSubtitleTitle)}});
    }
}

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
    connect(&m_subtitleNetwork, &QNetworkAccessManager::sslErrors, this,
            [this](QNetworkReply *reply, const QList<QSslError> &errors) {
                if (m_ignoreCertificateErrors)
                    reply->ignoreSslErrors(errors);
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
        cancelDownloadAll();
        for (auto it = m_fetches.cbegin(); it != m_fetches.cend(); ++it)
            m_client->releaseListingChannel(it.key());
        for (auto it = m_outboxFetches.cbegin(); it != m_outboxFetches.cend(); ++it)
            m_client->releaseListingChannel(it.key());
        for (auto it = m_outboxVerifies.cbegin(); it != m_outboxVerifies.cend(); ++it)
            m_client->releaseListingChannel(it.key());
        for (auto it = m_ruleFetches.cbegin(); it != m_ruleFetches.cend(); ++it)
            m_client->releaseListingChannel(it.key());
        for (auto it = m_extrasFetches.cbegin(); it != m_extrasFetches.cend(); ++it)
            m_client->releaseListingChannel(it.key());
        dropExtras();
        m_extrasFetches.clear();
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
    dropExtras();
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
    // CanDownload is opt-in: the server leaves it out of an item unless the fields ask for it
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

std::optional<DownloadRecord> DownloadsController::recordForPlanned(const QJsonObject &item) const
{
    if (item.value(QStringLiteral("Type")).toString() != QLatin1String("Episode")
        || item.value(QStringLiteral("LocationType")).toString() == QLatin1String("Virtual")
        || !item.value(QStringLiteral("MediaSources")).isArray()
        || item.value(QStringLiteral("MediaSources")).toArray().isEmpty())
        return std::nullopt;
    return recordFor(item, {});
}

QVariantMap DownloadsController::downloadAllPlan() const
{
    QVariantMap plan{{QStringLiteral("state"), m_all.state},
                     {QStringLiteral("containerId"), m_all.containerId},
                     {QStringLiteral("unwatchedOnly"), m_all.unwatchedOnly},
                     {QStringLiteral("truncated"), m_all.truncated},
                     {QStringLiteral("limit"), MaxDownloadAll},
                     {QStringLiteral("total"), m_all.total}};
    int count = 0;
    int already = 0;
    int watched = 0;
    qint64 size = 0;
    bool sizeKnown = true;
    for (const QJsonObject &item : m_all.items) {
        const std::optional<DownloadRecord> record = recordForPlanned(item);
        if (!record)
            continue;
        if (m_manager.store()->find(record->serverId, record->userId, record->itemId)) {
            ++already;
            continue;
        }
        const bool played = item.value(QStringLiteral("UserData"))
                                .toObject()
                                .value(QStringLiteral("Played"))
                                .toBool();
        if (m_all.unwatchedOnly && played) {
            ++watched;
            continue;
        }
        ++count;
        if (record->size > 0)
            size += record->size;
        else
            sizeKnown = false;
    }
    plan.insert(QStringLiteral("count"), count);
    plan.insert(QStringLiteral("alreadyDownloaded"), already);
    plan.insert(QStringLiteral("watchedSkipped"), watched);
    plan.insert(QStringLiteral("size"), size);
    plan.insert(QStringLiteral("sizeKnown"), sizeKnown && count > 0);
    return plan;
}

bool DownloadsController::planDownloadAll(const QString &containerId, bool series)
{
    cancelDownloadAll();
    if (!canDownload() || !isValidJellyfinId(containerId) || m_client->userId().isEmpty())
        return false;
    m_all.containerId = containerId;
    m_all.series = series;
    m_all.state = QStringLiteral("reading");
    m_all.channel = m_nextChannel++;
    requestDownloadAllPage();
    if (m_all.state != QLatin1String("reading"))
        return false;
    Q_EMIT downloadAllPlanChanged();
    return true;
}

void DownloadsController::requestDownloadAllPage()
{
    ItemQuery query;
    query.parentId = m_all.containerId;
    query.recursive = m_all.series;
    query.includeItemTypes = QStringLiteral("Episode");
    query.sortBy = m_all.series ? QStringLiteral("ParentIndexNumber,IndexNumber")
                                : QStringLiteral("IndexNumber");
    query.sortOrder = QStringLiteral("Ascending");
    query.startIndex = int(m_all.items.size());
    query.limit = DownloadAllPage;
    query.fields = {QStringLiteral("MediaSources"), QStringLiteral("CanDownload")};
    if (!m_client->fetchItems(query, m_all.channel))
        endDownloadAll(QStringLiteral("failed"));
}

void DownloadsController::onDownloadAllItems(const QJsonArray &items, int total)
{
    const qsizetype before = m_all.items.size();
    for (const QJsonValue &value : items) {
        if (value.isObject() && int(m_all.items.size()) < MaxDownloadAll)
            m_all.items.append(value.toObject());
    }
    m_all.total = int(m_all.items.size());
    // A page that adds nothing ends the reading, whatever total the server claims
    const bool more = m_all.items.size() > before && int(m_all.items.size()) < total;
    if (more && int(m_all.items.size()) >= MaxDownloadAll) {
        m_all.truncated = true;
    } else if (more) {
        requestDownloadAllPage();
        return;
    }
    m_client->releaseListingChannel(m_all.channel);
    m_all.channel = -1;
    m_all.state = QStringLiteral("ready");
    Q_EMIT downloadAllPlanChanged();
}

void DownloadsController::endDownloadAll(const QString &state)
{
    if (m_all.channel >= 0)
        m_client->releaseListingChannel(m_all.channel);
    m_all = DownloadAll();
    m_all.state = state;
    Q_EMIT downloadAllPlanChanged();
}

void DownloadsController::setDownloadAllUnwatchedOnly(bool unwatchedOnly)
{
    if (m_all.unwatchedOnly == unwatchedOnly)
        return;
    m_all.unwatchedOnly = unwatchedOnly;
    Q_EMIT downloadAllPlanChanged();
}

int DownloadsController::startDownloadAll()
{
    if (m_all.state != QLatin1String("ready") || !canDownload())
        return 0;
    int queued = 0;
    // The queue starts with the oldest: the batch keeps the order of the episodes by a millisecond each
    qint64 added = QDateTime::currentMSecsSinceEpoch();
    for (const QJsonObject &item : m_all.items) {
        std::optional<DownloadRecord> record = recordForPlanned(item);
        if (!record || m_manager.store()->find(record->serverId, record->userId, record->itemId))
            continue;
        const bool played = item.value(QStringLiteral("UserData"))
                                .toObject()
                                .value(QStringLiteral("Played"))
                                .toBool();
        if (m_all.unwatchedOnly && played)
            continue;
        // The index is written once the batch is in, not after each record
        record->addedAt = added++;
        if (m_manager.enqueue(*record, true))
            ++queued;
    }
    m_manager.store()->flush();
    m_manager.wake();
    m_all = DownloadAll();
    Q_EMIT downloadAllPlanChanged();
    return queued;
}

void DownloadsController::cancelDownloadAll()
{
    if (m_all.state == QLatin1String("idle") && m_all.channel < 0)
        return;
    endDownloadAll(QStringLiteral("idle"));
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
    const QString directory = QFileInfo(media).absolutePath();
    writePrivateFile(directory + QStringLiteral("/info.json"),
                     QJsonDocument(infoObject(record)).toJson(QJsonDocument::Compact));
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
    fetchExtras(record);
}

bool DownloadsController::signedInOwns(const DownloadRecord &record) const
{
    return record.serverId.compare(m_client->serverId(), Qt::CaseInsensitive) == 0
           && record.userId.compare(m_client->userId(), Qt::CaseInsensitive) == 0;
}

void DownloadsController::fetchExtras(const DownloadRecord &record)
{
    // Another user's record has no session to read it with
    if (!signedInOwns(record))
        return;
    ItemQuery query;
    query.itemIds = {record.itemId};
    query.fields = {QStringLiteral("Chapters"), QStringLiteral("MediaSources")};
    const int channel = m_nextChannel++;
    if (m_client->fetchItems(query, channel))
        m_extrasFetches.insert(channel, record.key());
}

void DownloadsController::onExtrasReady(const QJsonArray &items, int channel)
{
    const QString key = m_extrasFetches.take(channel);
    m_client->releaseListingChannel(channel);
    const std::optional<DownloadRecord> record = m_manager.store()->findByKey(key);
    if (!record || items.isEmpty() || m_manager.localPath(*record).isEmpty())
        return;
    // Only the item that was asked for
    const QJsonObject item = items.first().toObject();
    if (item.value(QStringLiteral("Id")).toString().compare(record->itemId, Qt::CaseInsensitive)
        != 0)
        return;
    DownloadExtras extras;
    extras.key = key;
    for (const ChapterSummary &chapter : parseChapters(item.value(QStringLiteral("Chapters")))) {
        if (chapter.startTicks > DownloadRecord::MaxRuntimeTicks)
            continue;
        extras.chapters.append(
            QJsonObject{{QStringLiteral("Name"), chapter.name},
                        {QStringLiteral("StartPositionTicks"), chapter.startTicks}});
    }
    // The downloaded file is the first media source
    const QJsonArray sources = item.value(QStringLiteral("MediaSources")).toArray();
    const QJsonObject source = sources.isEmpty() ? QJsonObject() : sources.first().toObject();
    const QString sourceId = source.value(QStringLiteral("Id")).toString();
    if (isValidJellyfinId(sourceId)) {
        extras.mediaSourceId = sourceId;
        QSet<int> seen;
        for (const QJsonValue &stream : source.value(QStringLiteral("MediaStreams")).toArray()) {
            if (extras.pending.size() == MaxSubtitleTracks)
                break;
            const std::optional<SubtitleTrack> track = subtitleTrackOf(stream.toObject());
            if (track && !seen.contains(track->index)) {
                seen.insert(track->index);
                extras.pending.append(*track);
            }
        }
    }
    if (extras.chapters.isEmpty() && extras.pending.isEmpty())
        return;
    m_extrasJobs.append(extras);
    advanceExtras();
}

void DownloadsController::advanceExtras()
{
    while (!m_subtitleReply && !m_extrasJobs.isEmpty()) {
        DownloadExtras &extras = m_extrasJobs.first();
        if (extras.pending.isEmpty()) {
            const DownloadExtras done = m_extrasJobs.takeFirst();
            finishExtras(done);
            continue;
        }
        const SubtitleTrack track = extras.pending.takeFirst();
        const std::optional<DownloadRecord> record = m_manager.store()->findByKey(extras.key);
        // The download may be gone, or the session another user's: nothing more is fetched for it
        if (!record || !signedInOwns(*record) || m_manager.localPath(*record).isEmpty()) {
            extras.pending.clear();
            continue;
        }
        const QUrl url = m_client->subtitleFileUrl(record->itemId, extras.mediaSourceId,
                                                   track.index, track.extension);
        const QByteArray authorization = m_client->streamAuthorization();
        if (url.isEmpty() || authorization.isEmpty())
            continue;
        QNetworkRequest request(url);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::ManualRedirectPolicy);
        request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                             QNetworkRequest::AlwaysNetwork);
        request.setTransferTimeout(30'000);
        request.setRawHeader("Accept", "*/*");
        request.setRawHeader("Accept-Encoding", "identity");
        request.setRawHeader("Authorization", authorization);
        m_subtitleBuffer.clear();
        m_subtitleOversize = false;
        QNetworkReply *reply = m_subtitleNetwork.get(request);
        m_subtitleReply = reply;
        // A bounded buffer keeps one network chunk from passing the cap before it is counted
        reply->setReadBufferSize(64 * 1024);
        // The transfer timeout restarts with every byte: a server that drips them would hold the only slot, so each file has a deadline too
        QTimer::singleShot(60'000, reply, [this, reply] {
            if (reply == m_subtitleReply)
                reply->abort();
        });
        connect(reply, &QNetworkReply::metaDataChanged, this, [this, reply] {
            if (reply == m_subtitleReply
                && reply->header(QNetworkRequest::ContentLengthHeader).toLongLong()
                       > MaxSubtitleBytes) {
                m_subtitleOversize = true;
                reply->abort();
            }
        });
        connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
            if (reply != m_subtitleReply)
                return;
            m_subtitleBuffer += reply->readAll();
            if (m_subtitleBuffer.size() > MaxSubtitleBytes) {
                m_subtitleOversize = true;
                m_subtitleBuffer.clear();
                reply->abort();
            }
        });
        const QString key = extras.key;
        connect(reply, &QNetworkReply::finished, this,
                [this, reply, key, track] { onSubtitleFinished(reply, key, track); });
    }
}

void DownloadsController::onSubtitleFinished(QNetworkReply *reply, const QString &key,
                                             const SubtitleTrack &track)
{
    if (reply != m_subtitleReply)
        return;
    m_subtitleReply.clear();
    reply->deleteLater();
    // An aborted reply is closed and has nothing more to read
    if (reply->isOpen())
        m_subtitleBuffer += reply->readAll();
    const bool oversize = m_subtitleOversize || m_subtitleBuffer.size() > MaxSubtitleBytes;
    const QByteArray data = std::exchange(m_subtitleBuffer, QByteArray());
    // Only a plain 200: a redirect is never followed, so the token never leaves the server
    const bool received =
        !oversize && !data.isEmpty() && reply->error() == QNetworkReply::NoError
        && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200;
    bool written = false;
    if (received) {
        // The download may have been removed meanwhile: write into the folder it still has, never create one
        const std::optional<DownloadRecord> current = m_manager.store()->findByKey(key);
        const QString media = current ? m_manager.localPath(*current) : QString();
        if (!media.isEmpty()) {
            const QString directory = QFileInfo(media).absolutePath();
            const QFileInfo subs(directory + QStringLiteral("/subs"));
            if (QFileInfo(directory).isDir() && !subs.isSymLink()
                && (!subs.exists() || subs.isDir()))
                written = writePrivateFile(subs.filePath() + QLatin1Char('/')
                                               + QString::number(track.index) + QLatin1Char('.')
                                               + track.extension,
                                           data);
        }
    }
    if (!written)
        qCInfo(downloadsLog) << "A subtitle file was not kept";
    else if (!m_extrasJobs.isEmpty() && m_extrasJobs.first().key == key)
        m_extrasJobs.first().written.append(track);
    advanceExtras();
}

void DownloadsController::finishExtras(const DownloadExtras &extras)
{
    if (extras.chapters.isEmpty() && extras.written.isEmpty())
        return;
    const std::optional<DownloadRecord> record = m_manager.store()->findByKey(extras.key);
    const QString media = record ? m_manager.localPath(*record) : QString();
    if (media.isEmpty())
        return;
    const QString directory = QFileInfo(media).absolutePath();
    if (!QFileInfo(directory).isDir())
        return;
    QJsonObject info = infoObject(*record);
    if (!extras.chapters.isEmpty())
        info.insert(QStringLiteral("chapters"), extras.chapters);
    if (!extras.written.isEmpty()) {
        QJsonArray subtitles;
        for (const SubtitleTrack &track : extras.written) {
            subtitles.append(
                QJsonObject{{QStringLiteral("index"), track.index},
                            {QStringLiteral("file"),
                             QStringLiteral("subs/%1.%2").arg(track.index).arg(track.extension)},
                            {QStringLiteral("language"), track.language},
                            {QStringLiteral("title"), track.title}});
        }
        info.insert(QStringLiteral("subtitles"), subtitles);
    }
    writePrivateFile(directory + QStringLiteral("/info.json"),
                     QJsonDocument(info).toJson(QJsonDocument::Compact));
}

void DownloadsController::dropExtras()
{
    m_extrasJobs.clear();
    m_subtitleBuffer.clear();
    if (QNetworkReply *reply = m_subtitleReply.data()) {
        m_subtitleReply.clear();
        // The handlers must not run for a reply that is being dropped
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
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
    const std::optional<DownloadRule> existing =
        m_rules.find(rule.serverId, rule.userId, rule.seriesId);
    // Editing a paused rule leaves it paused
    rule.enabled = existing ? existing->enabled : true;
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

bool DownloadsController::setSeriesRuleEnabled(const QString &seriesId, bool enabled)
{
    if (m_client->userId().isEmpty())
        return false;
    std::optional<DownloadRule> rule =
        m_rules.find(m_client->serverId(), m_client->userId(), seriesId);
    if (!rule)
        return false;
    if (rule->enabled == enabled)
        return true;
    rule->enabled = enabled;
    if (enabled) {
        rule->cursorDate = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        rule->cursorId.clear();
    }
    if (!m_rules.upsert(*rule))
        return false;
    if (enabled)
        scanRules();
    return true;
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
                                  {QStringLiteral("count"), rule.count},
                                  {QStringLiteral("enabled"), rule.enabled}});
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
    QVariantList chapters;
    QVariantList subtitles;
    readInfoExtras(QFileInfo(path).absolutePath(), &chapters, &subtitles);
    return {{QStringLiteral("path"), path},
            {QStringLiteral("name"), record->name},
            {QStringLiteral("type"), record->type},
            {QStringLiteral("seriesName"), record->seriesName},
            {QStringLiteral("seasonNumber"), record->seasonNumber},
            {QStringLiteral("episodeNumber"), record->episodeNumber},
            {QStringLiteral("positionTicks"), record->positionTicks},
            {QStringLiteral("chapters"), chapters},
            {QStringLiteral("subtitles"), subtitles}};
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
    // `POST /UserPlayedItems/{itemId}`, never `Sessions/Playing/Stopped`, which needs a live play session
    if (!m_client->setItemPlayed(event->itemId, true))
        m_outboxWrites.remove(event->itemId);
}

void DownloadsController::onItemsReady(const QJsonArray &items, int totalRecordCount, int channel)
{
    if (m_all.channel >= 0 && channel == m_all.channel) {
        onDownloadAllItems(items, totalRecordCount);
        return;
    }
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
    if (m_extrasFetches.contains(channel)) {
        onExtrasReady(items, channel);
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
    if (m_all.channel >= 0 && channel == m_all.channel) {
        endDownloadAll(QStringLiteral("failed"));
        return;
    }
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
    if (m_extrasFetches.contains(channel)) {
        // The info file stays as it is
        m_extrasFetches.remove(channel);
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
