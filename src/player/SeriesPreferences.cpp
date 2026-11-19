// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "SeriesPreferences.h"

#include "api/MediaSourceSelection.h"
#include "player/PlayerSettings.h"
#include "settings/ProfilePaths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocale>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>

namespace Kinefin {
namespace {
// More tracks than mpv lists for any file the player accepts
constexpr int MaxOrdinal = 1024;
// Above any count of choices, below the doubles JSON keeps exact
constexpr double MaxSeq = 1e15;

std::optional<TrackChoice> readTrack(const QJsonValue &value)
{
    if (!value.isObject())
        return std::nullopt;
    const QJsonObject object = value.toObject();
    TrackChoice choice;
    if (const QJsonValue lang = object.value(QStringLiteral("lang")); lang.isString())
        choice.lang = trackLanguage(lang.toString());
    if (const QJsonValue forced = object.value(QStringLiteral("forced")); forced.isBool())
        choice.forced = forced.toBool();
    if (const QJsonValue ordinal = object.value(QStringLiteral("ordinal"));
        ordinal.isDouble() && ordinal.toDouble() >= 0 && ordinal.toDouble() < MaxOrdinal
        && ordinal.toDouble() == std::floor(ordinal.toDouble()))
        choice.ordinal = ordinal.toInt();
    if (choice.lang.isEmpty() && choice.ordinal < 0)
        return std::nullopt;
    return choice;
}

QJsonObject trackObject(const TrackChoice &choice)
{
    QJsonObject object;
    if (!choice.lang.isEmpty())
        object.insert(QStringLiteral("lang"), choice.lang);
    if (choice.forced)
        object.insert(QStringLiteral("forced"), true);
    if (choice.ordinal >= 0)
        object.insert(QStringLiteral("ordinal"), choice.ordinal);
    return object;
}

bool isSubtitleOff(const QJsonValue &value)
{
    return value.isObject() && value.toObject().value(QStringLiteral("off")) == QJsonValue(true);
}

bool validRate(double rate)
{
    return std::isfinite(rate) && rate >= 0.5 && rate <= 4.0;
}

// What is well typed and valid in an entry stays, field by field
QJsonObject cleanEntry(const QJsonObject &entry)
{
    QJsonObject clean;
    if (const auto audio = readTrack(entry.value(QStringLiteral("audio"))))
        clean.insert(QStringLiteral("audio"), trackObject(*audio));
    const QJsonValue subtitle = entry.value(QStringLiteral("subtitle"));
    if (isSubtitleOff(subtitle))
        clean.insert(QStringLiteral("subtitle"), QJsonObject{{QStringLiteral("off"), true}});
    else if (const auto track = readTrack(subtitle))
        clean.insert(QStringLiteral("subtitle"), trackObject(*track));
    if (const auto track = readTrack(entry.value(QStringLiteral("subtitle2"))))
        clean.insert(QStringLiteral("subtitle2"), trackObject(*track));
    if (const QJsonValue rate = entry.value(QStringLiteral("rate"));
        rate.isDouble() && validRate(rate.toDouble()))
        clean.insert(QStringLiteral("rate"), rate.toDouble());
    if (const QJsonValue aspect = entry.value(QStringLiteral("aspect"));
        aspect.isString() && PlayerSettings::isAspectMode(aspect.toString()))
        clean.insert(QStringLiteral("aspect"), aspect.toString());
    if (clean.isEmpty())
        return clean;
    const QJsonValue seq = entry.value(QStringLiteral("seq"));
    clean.insert(QStringLiteral("seq"),
                 seq.isDouble() && seq.toDouble() >= 0 && seq.toDouble() <= MaxSeq
                     ? std::floor(seq.toDouble())
                     : 0.0);
    return clean;
}

void evictOldest(QJsonObject &entries)
{
    QString oldest;
    double lowest = MaxSeq + 1;
    for (auto it = entries.constBegin(); it != entries.constEnd(); ++it) {
        const double seq = it.value().toObject().value(QStringLiteral("seq")).toDouble();
        if (seq < lowest) {
            lowest = seq;
            oldest = it.key();
        }
    }
    entries.remove(oldest);
}

QLocale::Language languageOf(const QString &code)
{
    const qsizetype dash = code.indexOf(QLatin1Char('-'));
    return QLocale::codeToLanguage(QStringView(code).left(dash < 0 ? code.size() : dash));
}

bool sameLanguage(const QString &a, const QString &b)
{
    if (a.isEmpty() || b.isEmpty())
        return false;
    const QLocale::Language first = languageOf(a);
    const QLocale::Language second = languageOf(b);
    return first != QLocale::AnyLanguage && second != QLocale::AnyLanguage ? first == second
                                                                           : a == b;
}
} // namespace

QString trackLanguage(const QString &code)
{
    static const QRegularExpression pattern(QStringLiteral("^[a-z]{2,3}(-[a-z0-9]{1,8})?\\z"));
    const QString lower = code.toLower();
    // Undetermined says nothing: the track is found again by its place
    return lower != QLatin1String("und") && pattern.match(lower).hasMatch() ? lower : QString();
}

int pickTrack(const QList<TrackFact> &tracks, const TrackChoice &choice)
{
    if (choice.lang.isEmpty())
        return choice.ordinal >= 0 && choice.ordinal < tracks.size() ? tracks.at(choice.ordinal).id
                                                                     : 0;
    for (const TrackFact &track : tracks) {
        if (sameLanguage(track.lang, choice.lang) && track.forced == choice.forced)
            return track.id;
    }
    for (const TrackFact &track : tracks) {
        if (sameLanguage(track.lang, choice.lang))
            return track.id;
    }
    return 0;
}

SeriesPreferences::SeriesPreferences(QObject *parent) : QObject(parent)
{
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(std::chrono::seconds(1));
    connect(&m_saveTimer, &QTimer::timeout, this, &SeriesPreferences::save);
}

SeriesPreferences::~SeriesPreferences()
{
    flush();
}

void SeriesPreferences::setDirectory(const QString &directory)
{
    flush();
    m_directory = directory;
    load();
}

void SeriesPreferences::setSaveDelay(std::chrono::milliseconds delay)
{
    m_saveTimer.setInterval(delay);
}

void SeriesPreferences::setOwner(const QString &serverId, const QString &userId)
{
    const bool valid = isValidJellyfinId(serverId) && isValidJellyfinId(userId);
    const QString server = valid ? serverId.toLower() : QString();
    const QString user = valid ? userId.toLower() : QString();
    if (server == m_serverId && user == m_userId)
        return;
    flush();
    m_serverId = server;
    m_userId = user;
    load();
    Q_EMIT changed();
}

QString SeriesPreferences::ownerFile(const QString &serverId, const QString &userId) const
{
    // Valid ids hold no path separator and no dot: the name cannot leave the folder
    if (m_directory.isEmpty() || !isValidJellyfinId(serverId) || !isValidJellyfinId(userId))
        return {};
    return m_directory + QLatin1Char('/') + serverId.toLower() + QLatin1Char('_') + userId.toLower()
           + QStringLiteral(".json");
}

void SeriesPreferences::load()
{
    m_entries = {};
    m_seq = 0;
    const QString path = ownerFile(m_serverId, m_userId);
    if (path.isEmpty())
        return;
    // A link (a dangling one too), a pipe or a device is no file of choices: it is never opened
    const QFileInfo info(path);
    if (!info.isSymLink() && !info.exists())
        return;
    if (info.isSymLink() || !info.isFile() || info.size() > MaxFileBytes) {
        QFile::remove(path);
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return;
    // The file may have grown since it was measured: one byte past the limit is enough to tell
    const QByteArray bytes = file.read(MaxFileBytes + 1);
    file.close();
    const QJsonObject root = QJsonDocument::fromJson(bytes).object();
    const QJsonValue version = root.value(QStringLiteral("version"));
    if (version.isDouble() && version.toInt() > FileVersion) {
        // A later version wrote it: kept aside rather than lost
        QFile::remove(path + QStringLiteral(".broken"));
        QFile::rename(path, path + QStringLiteral(".broken"));
        return;
    }
    const QJsonValue entries = root.value(QStringLiteral("entries"));
    if (bytes.size() > MaxFileBytes || !version.isDouble() || version.toInt() != FileVersion
        || !entries.isObject()) {
        QFile::remove(path);
        return;
    }
    const QJsonObject stored = entries.toObject();
    for (auto it = stored.constBegin(); it != stored.constEnd(); ++it) {
        if (!isValidJellyfinId(it.key()) || !it.value().isObject())
            continue;
        const QJsonObject entry = cleanEntry(it.value().toObject());
        if (!entry.isEmpty())
            m_entries.insert(it.key().toLower(), entry);
    }
    while (m_entries.size() > MaxSeries)
        evictOldest(m_entries);
    // Numbered again from 1 in the stored order, so a file's large numbers never reach the cap
    QList<std::pair<double, QString>> order;
    for (auto it = m_entries.constBegin(); it != m_entries.constEnd(); ++it)
        order.append({it.value().toObject().value(QStringLiteral("seq")).toDouble(), it.key()});
    std::sort(order.begin(), order.end());
    for (const auto &[seq, key] : std::as_const(order)) {
        QJsonObject entry = m_entries.value(key).toObject();
        entry.insert(QStringLiteral("seq"), double(++m_seq));
        m_entries.insert(key, entry);
    }
}

bool SeriesPreferences::contains(const QString &seriesId) const
{
    return m_entries.contains(seriesId.toLower());
}

SeriesChoice SeriesPreferences::choice(const QString &seriesId) const
{
    const QJsonObject entry = m_entries.value(seriesId.toLower()).toObject();
    SeriesChoice choice;
    choice.audio = readTrack(entry.value(QStringLiteral("audio")));
    const QJsonValue subtitle = entry.value(QStringLiteral("subtitle"));
    choice.subtitleOff = isSubtitleOff(subtitle);
    if (!choice.subtitleOff)
        choice.subtitle = readTrack(subtitle);
    choice.subtitle2 = readTrack(entry.value(QStringLiteral("subtitle2")));
    if (const QJsonValue rate = entry.value(QStringLiteral("rate")); rate.isDouble())
        choice.rate = rate.toDouble();
    choice.aspect = entry.value(QStringLiteral("aspect")).toString();
    return choice;
}

void SeriesPreferences::update(const QString &seriesId, const QString &field,
                               const QJsonValue &value)
{
    if (!isValidJellyfinId(seriesId) || m_serverId.isEmpty())
        return;
    const QString key = seriesId.toLower();
    QJsonObject entry = m_entries.value(key).toObject();
    if (value.isUndefined()) {
        if (!entry.contains(field))
            return;
        entry.remove(field);
    } else {
        entry.insert(field, value);
    }
    entry.insert(QStringLiteral("seq"), double(++m_seq));
    entry = cleanEntry(entry);
    if (entry.isEmpty()) {
        m_entries.remove(key);
    } else {
        m_entries.insert(key, entry);
        while (m_entries.size() > MaxSeries)
            evictOldest(m_entries);
    }
    m_saveTimer.start();
    Q_EMIT changed();
}

void SeriesPreferences::setAudio(const QString &seriesId, const std::optional<TrackChoice> &audio)
{
    update(seriesId, QStringLiteral("audio"),
           audio ? QJsonValue(trackObject(*audio)) : QJsonValue(QJsonValue::Undefined));
}

void SeriesPreferences::setSubtitle(const QString &seriesId, bool off,
                                    const std::optional<TrackChoice> &subtitle)
{
    update(seriesId, QStringLiteral("subtitle"),
           off        ? QJsonValue(QJsonObject{{QStringLiteral("off"), true}})
           : subtitle ? QJsonValue(trackObject(*subtitle))
                      : QJsonValue(QJsonValue::Undefined));
}

void SeriesPreferences::setSecondarySubtitle(const QString &seriesId,
                                             const std::optional<TrackChoice> &subtitle)
{
    update(seriesId, QStringLiteral("subtitle2"),
           subtitle ? QJsonValue(trackObject(*subtitle)) : QJsonValue(QJsonValue::Undefined));
}

void SeriesPreferences::setRate(const QString &seriesId, std::optional<double> rate)
{
    update(seriesId, QStringLiteral("rate"),
           rate && validRate(*rate) ? QJsonValue(*rate) : QJsonValue(QJsonValue::Undefined));
}

void SeriesPreferences::setAspect(const QString &seriesId, const QString &aspect)
{
    update(seriesId, QStringLiteral("aspect"),
           PlayerSettings::isAspectMode(aspect) ? QJsonValue(aspect)
                                                : QJsonValue(QJsonValue::Undefined));
}

void SeriesPreferences::forget(const QString &seriesId)
{
    if (!m_entries.contains(seriesId.toLower()))
        return;
    m_entries.remove(seriesId.toLower());
    m_saveTimer.start();
    Q_EMIT changed();
}

void SeriesPreferences::flush()
{
    if (!m_saveTimer.isActive())
        return;
    m_saveTimer.stop();
    save();
}

void SeriesPreferences::save()
{
    const QString path = ownerFile(m_serverId, m_userId);
    if (path.isEmpty())
        return;
    QByteArray bytes;
    bool evicted = false;
    while (true) {
        bytes = QJsonDocument(QJsonObject{{QStringLiteral("version"), FileVersion},
                                          {QStringLiteral("entries"), m_entries}})
                    .toJson(QJsonDocument::Compact);
        if (bytes.size() <= MaxFileBytes || m_entries.isEmpty())
            break;
        evictOldest(m_entries);
        evicted = true;
    }
    if (evicted)
        Q_EMIT changed();
    // No choice left, no file; a link in the file's place is removed, never written through
    if (m_entries.isEmpty() || QFileInfo(path).isSymLink())
        QFile::remove(path);
    // A failed write loses a convenience, not data: the choices stay in memory for this session and are written again with the next one
    if (!m_entries.isEmpty())
        writePrivateFile(path, bytes);
}

void SeriesPreferences::forgetUser(const QString &serverId, const QString &userId)
{
    if (const QString path = ownerFile(serverId, userId); !path.isEmpty()) {
        QFile::remove(path);
        QFile::remove(path + QStringLiteral(".broken"));
    }
    if (serverId.toLower() != m_serverId || userId.toLower() != m_userId || m_serverId.isEmpty())
        return;
    m_saveTimer.stop();
    m_entries = {};
    Q_EMIT changed();
}

void SeriesPreferences::forgetServer(const QString &serverId)
{
    if (!isValidJellyfinId(serverId))
        return;
    if (!m_directory.isEmpty()) {
        QDir directory(m_directory);
        const QStringList files =
            directory.entryList({serverId.toLower() + QStringLiteral("_*.json"),
                                 serverId.toLower() + QStringLiteral("_*.json.broken")},
                                QDir::Files | QDir::Hidden | QDir::System);
        for (const QString &name : files)
            directory.remove(name);
    }
    if (serverId.toLower() != m_serverId)
        return;
    m_saveTimer.stop();
    m_entries = {};
    Q_EMIT changed();
}
} // namespace Kinefin
