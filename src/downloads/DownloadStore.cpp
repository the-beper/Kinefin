// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "DownloadStore.h"

#include "api/MediaSourceSelection.h"
#include "settings/ProfilePaths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <algorithm>
#include <array>
#include <cmath>

namespace Kinefin {
namespace {
constexpr qint64 MaxTimestamp = qint64(1) << 52;
constexpr qint64 MaxMediaBytes = qint64(1) << 50;
constexpr int MaxYear = 9'999;

const std::array kStates{QLatin1String("queued"),  QLatin1String("fetching"),
                         QLatin1String("paused"),  QLatin1String("waiting"),
                         QLatin1String("error"),   QLatin1String("completed"),
                         QLatin1String("exported")};
const std::array kErrors{QLatin1String("network"),  QLatin1String("http"),
                         QLatin1String("protocol"), QLatin1String("size"),
                         QLatin1String("changed"),  QLatin1String("io"),
                         QLatin1String("nospace"),  QLatin1String("cancelled")};
const std::array kTypes{QLatin1String("Movie"), QLatin1String("Episode"), QLatin1String("Video")};

template <typename Tokens> bool oneOf(const Tokens &allowed, const QString &token)
{
    return std::any_of(allowed.cbegin(), allowed.cend(),
                       [&token](QLatin1String entry) { return token == entry; });
}

// Remote text kept as it is (no trimming): non-empty within `max`, no control character, no line separator
bool validText(const QString &text, int max, bool allowEmpty = false)
{
    if (text.isEmpty())
        return allowEmpty;
    if (text.size() > max)
        return false;
    for (const QChar c : text) {
        const char16_t u = c.unicode();
        if (u < 0x20 || (u >= 0x7f && u <= 0x9f) || u == 0x2028 || u == 0x2029)
            return false;
    }
    return true;
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

bool validEtag(const QString &etag)
{
    if (etag.size() > DownloadRecord::MaxEtag)
        return false;
    return std::all_of(etag.cbegin(), etag.cend(), [](QChar c) {
        const char16_t u = c.unicode();
        return u >= 0x20 && u != 0x7f;
    });
}

void normalise(DownloadRecord *record)
{
    record->serverId = record->serverId.toLower();
    record->userId = record->userId.toLower();
    record->itemId = record->itemId.toLower();
    record->seriesId = record->seriesId.toLower();
    record->seasonId = record->seasonId.toLower();
    record->state = record->state.toLower();
    record->error = record->error.toLower();
}

bool validRecord(const DownloadRecord &record)
{
    if (!isValidJellyfinId(record.serverId) || !isValidJellyfinId(record.userId)
        || !isValidJellyfinId(record.itemId))
        return false;
    if (!record.seriesId.isEmpty() && !isValidJellyfinId(record.seriesId))
        return false;
    if (!record.seasonId.isEmpty() && !isValidJellyfinId(record.seasonId))
        return false;
    if (!record.ruleId.isEmpty() && !isValidJellyfinId(record.ruleId))
        return false;
    if (!validText(record.name, DownloadRecord::MaxName)
        || !validText(record.seriesName, DownloadRecord::MaxName, true)
        || !validText(record.primaryTag, DownloadRecord::MaxTag, true))
        return false;
    if (!oneOf(kTypes, record.type) || !validContainer(record.container)
        || !oneOf(kStates, record.state) || !validEtag(record.etag))
        return false;
    if ((!record.error.isEmpty() && !oneOf(kErrors, record.error))
        || (record.state != QLatin1String("error") && !record.error.isEmpty()))
        return false;
    if (record.httpStatus < 0 || record.httpStatus > 599)
        return false;
    if (record.seasonNumber < -1 || record.seasonNumber > DownloadRecord::MaxIndexNumber
        || record.episodeNumber < -1 || record.episodeNumber > DownloadRecord::MaxIndexNumber
        || record.year < 0 || record.year > MaxYear)
        return false;
    if (record.runtimeTicks < 0 || record.runtimeTicks > DownloadRecord::MaxRuntimeTicks
        || record.size < -1 || record.size > MaxMediaBytes || record.bytesWritten < 0
        || record.bytesWritten > MaxMediaBytes
        || (record.size >= 0 && record.bytesWritten > record.size) || record.positionTicks < 0
        || record.positionTicks > DownloadRecord::MaxRuntimeTicks)
        return false;
    if (record.addedAt < 0 || record.addedAt > MaxTimestamp || record.updatedAt < 0
        || record.updatedAt > MaxTimestamp)
        return false;
    return true;
}

bool readString(const QJsonObject &object, QLatin1String key, QString *out)
{
    const QJsonValue value = object.value(key);
    if (!value.isString())
        return false;
    *out = value.toString();
    return true;
}

bool readInteger(const QJsonObject &object, QLatin1String key, qint64 minimum, qint64 maximum,
                 qint64 *out)
{
    const QJsonValue value = object.value(key);
    if (!value.isDouble())
        return false;
    const double number = value.toDouble();
    if (number != std::floor(number) || number < double(minimum) || number > double(maximum))
        return false;
    *out = qint64(number);
    return true;
}

std::optional<DownloadRecord> readRecord(const QJsonObject &object)
{
    DownloadRecord record;
    qint64 number = 0;
    if (!readString(object, QLatin1String("serverId"), &record.serverId)
        || !readString(object, QLatin1String("userId"), &record.userId)
        || !readString(object, QLatin1String("itemId"), &record.itemId)
        || !readString(object, QLatin1String("name"), &record.name)
        || !readString(object, QLatin1String("seriesName"), &record.seriesName)
        || !readString(object, QLatin1String("seriesId"), &record.seriesId)
        || !readString(object, QLatin1String("seasonId"), &record.seasonId)
        || !readString(object, QLatin1String("type"), &record.type)
        || !readString(object, QLatin1String("container"), &record.container)
        || !readString(object, QLatin1String("etag"), &record.etag)
        || !readString(object, QLatin1String("state"), &record.state)
        || !readString(object, QLatin1String("error"), &record.error)
        || !readString(object, QLatin1String("ruleId"), &record.ruleId))
        return std::nullopt;
    if (!readInteger(object, QLatin1String("httpStatus"), 0, 599, &number))
        return std::nullopt;
    record.httpStatus = int(number);
    if (!readInteger(object, QLatin1String("seasonNumber"), -1, DownloadRecord::MaxIndexNumber,
                     &number))
        return std::nullopt;
    record.seasonNumber = int(number);
    if (!readInteger(object, QLatin1String("episodeNumber"), -1, DownloadRecord::MaxIndexNumber,
                     &number))
        return std::nullopt;
    record.episodeNumber = int(number);
    if (!readInteger(object, QLatin1String("year"), 0, MaxYear, &number))
        return std::nullopt;
    record.year = int(number);
    if (!readInteger(object, QLatin1String("runtimeTicks"), 0, DownloadRecord::MaxRuntimeTicks,
                     &number))
        return std::nullopt;
    record.runtimeTicks = number;
    if (!readInteger(object, QLatin1String("size"), -1, MaxMediaBytes, &number))
        return std::nullopt;
    record.size = number;
    if (!readInteger(object, QLatin1String("bytesWritten"), 0, MaxMediaBytes, &number))
        return std::nullopt;
    record.bytesWritten = number;
    if (!readInteger(object, QLatin1String("addedAt"), 0, MaxTimestamp, &number))
        return std::nullopt;
    record.addedAt = number;
    if (!readInteger(object, QLatin1String("updatedAt"), 0, MaxTimestamp, &number))
        return std::nullopt;
    record.updatedAt = number;
    const QJsonValue wanted = object.value(QLatin1String("wanted"));
    if (!wanted.isBool())
        return std::nullopt;
    record.wanted = wanted.toBool();
    const QJsonValue meteredOverride = object.value(QLatin1String("meteredOverride"));
    if (!meteredOverride.isBool())
        return std::nullopt;
    record.meteredOverride = meteredOverride.toBool();
    // An index written before the poster existed has no tag: the row falls back to its type icon
    const QJsonValue positionTicks = object.value(QLatin1String("positionTicks"));
    if (positionTicks.isDouble())
        record.positionTicks = qint64(positionTicks.toDouble());
    const QJsonValue played = object.value(QLatin1String("played"));
    if (played.isBool())
        record.played = played.toBool();
    const QJsonValue primaryTag = object.value(QLatin1String("primaryTag"));
    if (primaryTag.isString())
        record.primaryTag = primaryTag.toString();
    normalise(&record);
    if (!validRecord(record))
        return std::nullopt;
    return record;
}

QJsonObject recordObject(const DownloadRecord &record)
{
    return QJsonObject{{QStringLiteral("serverId"), record.serverId},
                       {QStringLiteral("userId"), record.userId},
                       {QStringLiteral("itemId"), record.itemId},
                       {QStringLiteral("name"), record.name},
                       {QStringLiteral("seriesName"), record.seriesName},
                       {QStringLiteral("seriesId"), record.seriesId},
                       {QStringLiteral("seasonId"), record.seasonId},
                       {QStringLiteral("type"), record.type},
                       {QStringLiteral("container"), record.container},
                       {QStringLiteral("etag"), record.etag},
                       {QStringLiteral("primaryTag"), record.primaryTag},
                       {QStringLiteral("state"), record.state},
                       {QStringLiteral("error"), record.error},
                       {QStringLiteral("httpStatus"), record.httpStatus},
                       {QStringLiteral("seasonNumber"), record.seasonNumber},
                       {QStringLiteral("episodeNumber"), record.episodeNumber},
                       {QStringLiteral("year"), record.year},
                       {QStringLiteral("runtimeTicks"), record.runtimeTicks},
                       {QStringLiteral("size"), record.size},
                       {QStringLiteral("bytesWritten"), record.bytesWritten},
                       {QStringLiteral("positionTicks"), record.positionTicks},
                       {QStringLiteral("played"), record.played},
                       {QStringLiteral("addedAt"), record.addedAt},
                       {QStringLiteral("updatedAt"), record.updatedAt},
                       {QStringLiteral("ruleId"), record.ruleId},
                       {QStringLiteral("wanted"), record.wanted},
                       {QStringLiteral("meteredOverride"), record.meteredOverride}};
}

qint64 recordBytes(const DownloadRecord &record)
{
    return QJsonDocument(recordObject(record)).toJson(QJsonDocument::Compact).size();
}

// An entry with the same ids but a different case is the same item
int indexOfRecord(const QList<DownloadRecord> &records, const QString &key)
{
    for (int row = 0; row < records.size(); ++row) {
        if (records.at(row).key() == key)
            return row;
    }
    return -1;
}
} // namespace

QString DownloadRecord::key() const
{
    return serverId.toLower() + QLatin1Char('/') + userId.toLower() + QLatin1Char('/')
           + itemId.toLower();
}

DownloadStore::DownloadStore(QObject *parent) : QObject(parent)
{
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(500);
    connect(&m_saveTimer, &QTimer::timeout, this, [this] {
        if (m_dirty)
            write();
    });
}

DownloadStore::~DownloadStore()
{
    flush();
}

void DownloadStore::setStoragePath(const QString &path)
{
    flush();
    m_path = path;
    m_records.clear();
    m_bytes = serialize().size();
    m_dirty = false;
    if (!m_path.isEmpty())
        load();
}

QString DownloadStore::storagePath() const
{
    return m_path;
}

QString DownloadStore::rootDirectory() const
{
    return m_path.isEmpty() ? QString() : QFileInfo(m_path).absolutePath();
}

QList<DownloadRecord> DownloadStore::records() const
{
    return m_records;
}

std::optional<DownloadRecord> DownloadStore::find(const QString &serverId, const QString &userId,
                                                  const QString &itemId) const
{
    const QString key = serverId.toLower() + QLatin1Char('/') + userId.toLower() + QLatin1Char('/')
                        + itemId.toLower();
    const int row = indexOfRecord(m_records, key);
    if (row < 0)
        return std::nullopt;
    return m_records.at(row);
}

std::optional<DownloadRecord> DownloadStore::findByKey(const QString &key) const
{
    const int row = indexOfRecord(m_records, key.toLower());
    if (row < 0)
        return std::nullopt;
    return m_records.at(row);
}

bool DownloadStore::upsert(const DownloadRecord &record, bool writeNow)
{
    DownloadRecord stored = record;
    normalise(&stored);
    if (!validRecord(stored))
        return false;
    const QString key = stored.key();
    const qint64 storedBytes = recordBytes(stored);
    // A refused update has to leave the previous record exactly as it was, evictions included
    const QList<DownloadRecord> previous = m_records;
    const qint64 previousBytes = m_bytes;
    const bool previousDirty = m_dirty;
    const int row = indexOfRecord(m_records, key);
    if (row >= 0) {
        m_bytes += storedBytes - recordBytes(m_records.at(row));
        m_records.replace(row, stored);
    } else {
        while (m_records.size() >= MaxRecords) {
            if (!evictCompleted()) {
                m_records = previous;
                m_bytes = previousBytes;
                m_dirty = previousDirty;
                return false;
            }
        }
        if (!m_records.isEmpty())
            ++m_bytes;
        m_bytes += storedBytes;
        m_records.append(stored);
    }
    // Server text can make the file larger than the cap: give up completed entries, oldest first, never the record being stored
    while (m_bytes > MaxFileBytes && evictCompleted(key)) {
    }
    if (m_bytes > MaxFileBytes) {
        m_records = previous;
        m_bytes = previousBytes;
        m_dirty = previousDirty;
        return false;
    }
    m_dirty = true;
    if (writeNow) {
        m_saveTimer.stop();
        write();
    } else {
        scheduleSave();
    }
    return true;
}

bool DownloadStore::remove(const QString &serverId, const QString &userId, const QString &itemId,
                           bool writeNow)
{
    const QString key = serverId.toLower() + QLatin1Char('/') + userId.toLower() + QLatin1Char('/')
                        + itemId.toLower();
    const int row = indexOfRecord(m_records, key);
    if (row < 0)
        return true;
    m_bytes -= recordBytes(m_records.at(row));
    if (m_records.size() > 1)
        --m_bytes;
    m_records.removeAt(row);
    m_dirty = true;
    if (writeNow) {
        m_saveTimer.stop();
        write();
    } else {
        scheduleSave();
    }
    return true;
}

void DownloadStore::flush()
{
    m_saveTimer.stop();
    if (m_dirty)
        write();
}

bool DownloadStore::evictCompleted(const QString &keep)
{
    int oldest = -1;
    qint64 oldestStamp = 0;
    for (int row = 0; row < m_records.size(); ++row) {
        const DownloadRecord &record = m_records.at(row);
        if (record.key() == keep)
            continue;
        if (record.state != QLatin1String("completed") && record.state != QLatin1String("exported"))
            continue;
        if (oldest < 0 || record.updatedAt < oldestStamp) {
            oldest = row;
            oldestStamp = record.updatedAt;
        }
    }
    if (oldest < 0)
        return false;
    m_bytes -= recordBytes(m_records.at(oldest));
    if (m_records.size() > 1)
        --m_bytes;
    m_records.removeAt(oldest);
    m_dirty = true;
    return true;
}

void DownloadStore::scheduleSave()
{
    m_dirty = true;
    m_saveTimer.start();
}

QByteArray DownloadStore::serialize() const
{
    QJsonArray records;
    for (const DownloadRecord &record : m_records)
        records.append(recordObject(record));
    return QJsonDocument(QJsonObject{{QStringLiteral("version"), FileVersion},
                                     {QStringLiteral("records"), records}})
        .toJson(QJsonDocument::Compact);
}

bool DownloadStore::write()
{
    if (!m_dirty)
        return true;
    if (m_path.isEmpty()) {
        m_dirty = false;
        return true;
    }
    const QByteArray data = serialize();
    QString error;
    if (data.size() > MaxFileBytes || !writePrivateFile(m_path, data, &error)) {
        if (!m_saveFailing)
            Q_EMIT saveFailed();
        m_saveFailing = true;
        // Keep the flag: the next change, or flush(), tries again
        return false;
    }
    if (m_saveFailing) {
        m_saveFailing = false;
        Q_EMIT saveSucceeded();
    }
    m_dirty = false;
    return true;
}

void DownloadStore::load()
{
    const QFileInfo info(m_path);
    if (!info.exists())
        return;
    const auto setAside = [this, &info] {
        m_records.clear();
        const QString broken = m_path + QStringLiteral(".broken");
        QFile::remove(broken);
        QFile::rename(info.absoluteFilePath(), broken);
    };
    if (info.isSymLink() || !info.isFile() || info.size() > MaxFileBytes) {
        setAside();
        return;
    }
    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly)) {
        setAside();
        return;
    }
    const QByteArray data = file.read(MaxFileBytes + 1);
    file.close();
    if (data.size() > MaxFileBytes) {
        setAside();
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        setAside();
        return;
    }
    const QJsonObject root = document.object();
    const QJsonValue version = root.value(QStringLiteral("version"));
    const QJsonValue records = root.value(QStringLiteral("records"));
    if (!version.isDouble() || version.toDouble() != FileVersion || !records.isArray()) {
        setAside();
        return;
    }
    QSet<QString> seen;
    for (const QJsonValue &value : records.toArray()) {
        if (!value.isObject())
            continue;
        std::optional<DownloadRecord> record = readRecord(value.toObject());
        if (!record || seen.contains(record->key()))
            continue;
        if (m_records.size() >= MaxRecords)
            break;
        seen.insert(record->key());
        m_records.append(*record);
    }
    m_bytes = serialize().size();
    // A file that grew past the cap through its records alone is cut down to its completed entries' room, then to the oldest records, but never rewritten on load
    while (m_bytes > MaxFileBytes && evictCompleted()) {
    }
    while (m_bytes > MaxFileBytes && !m_records.isEmpty()) {
        m_bytes -= recordBytes(m_records.constFirst());
        if (m_records.size() > 1)
            --m_bytes;
        m_records.removeFirst();
    }
    m_dirty = false;
}
} // namespace Kinefin
