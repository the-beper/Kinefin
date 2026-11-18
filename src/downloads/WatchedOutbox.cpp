// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "WatchedOutbox.h"

#include "api/MediaSourceSelection.h"
#include "settings/ProfilePaths.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <utility>

namespace Kinefin {
namespace {
constexpr qint64 MaxTimestamp = qint64(1) << 52;
constexpr int MaxEventId = 64;

bool validEvent(const WatchedEvent &event)
{
    return isValidJellyfinId(event.serverId) && isValidJellyfinId(event.userId)
           && isValidJellyfinId(event.itemId) && !event.eventId.isEmpty()
           && event.eventId.size() <= MaxEventId && event.createdAt >= 0
           && event.createdAt <= MaxTimestamp;
}

QJsonObject eventObject(const WatchedEvent &event)
{
    return {
        {QStringLiteral("serverId"), event.serverId},   {QStringLiteral("userId"), event.userId},
        {QStringLiteral("itemId"), event.itemId},       {QStringLiteral("eventId"), event.eventId},
        {QStringLiteral("createdAt"), event.createdAt}, {QStringLiteral("played"), event.played}};
}

std::optional<WatchedEvent> readEvent(const QJsonObject &object)
{
    WatchedEvent event;
    const auto text = [&object](QLatin1String key, QString *out) {
        const QJsonValue value = object.value(key);
        if (!value.isString())
            return false;
        *out = value.toString();
        return true;
    };
    qint64 createdAt = 0;
    const QJsonValue created = object.value(QLatin1String("createdAt"));
    if (!text(QLatin1String("serverId"), &event.serverId)
        || !text(QLatin1String("userId"), &event.userId)
        || !text(QLatin1String("itemId"), &event.itemId)
        || !text(QLatin1String("eventId"), &event.eventId) || !created.isDouble())
        return std::nullopt;
    createdAt = qint64(created.toDouble());
    if (createdAt < 0 || createdAt > MaxTimestamp)
        return std::nullopt;
    event.createdAt = createdAt;
    const QJsonValue played = object.value(QLatin1String("played"));
    event.played = played.isBool() ? played.toBool() : true;
    event.conflict = object.value(QLatin1String("conflict")).toBool(false);
    if (!validEvent(event))
        return std::nullopt;
    return event;
}
} // namespace

QString WatchedEvent::key() const
{
    return serverId.toLower() + QLatin1Char('/') + userId.toLower() + QLatin1Char('/')
           + itemId.toLower();
}

WatchedOutbox::WatchedOutbox(QObject *parent) : QObject(parent) {}

WatchedOutbox::~WatchedOutbox()
{
    flush();
}

void WatchedOutbox::setStoragePath(const QString &path)
{
    flush();
    m_path = path;
    m_events.clear();
    m_dirty = false;
    if (!m_path.isEmpty())
        load();
}

void WatchedOutbox::load()
{
    QFile file(m_path);
    if (!file.exists())
        return;
    const QFileInfo info(m_path);
    const auto setAside = [this, &info] {
        m_events.clear();
        const QString broken = m_path + QStringLiteral(".broken");
        QFile::remove(broken);
        QFile::rename(info.absoluteFilePath(), broken);
    };
    if (info.isSymLink() || !info.isFile() || info.size() > 512 * 1024) {
        setAside();
        return;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        setAside();
        return;
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()
        || document.object().value(QStringLiteral("version")).toInt() != FileVersion) {
        setAside();
        return;
    }
    const QJsonArray events = document.object().value(QStringLiteral("events")).toArray();
    for (const QJsonValue &entry : events) {
        if (!entry.isObject())
            continue;
        const std::optional<WatchedEvent> event = readEvent(entry.toObject());
        if (!event)
            continue;
        bool replaced = false;
        for (WatchedEvent &existing : m_events) {
            if (existing.key() == event->key()) {
                existing = *event;
                replaced = true;
                break;
            }
        }
        if (!replaced && m_events.size() < MaxEvents)
            m_events.append(*event);
    }
}

std::optional<WatchedEvent> WatchedOutbox::find(const QString &serverId, const QString &userId,
                                                const QString &itemId) const
{
    const QString key = serverId.toLower() + QLatin1Char('/') + userId.toLower() + QLatin1Char('/')
                        + itemId.toLower();
    for (const WatchedEvent &event : m_events) {
        if (event.key() == key)
            return event;
    }
    return std::nullopt;
}

bool WatchedOutbox::append(const WatchedEvent &event)
{
    if (!validEvent(event))
        return false;
    bool replaced = false;
    for (WatchedEvent &existing : m_events) {
        if (existing.key() == event.key()) {
            existing = event;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        if (m_events.size() >= MaxEvents)
            m_events.removeFirst();
        m_events.append(event);
    }
    m_dirty = true;
    const bool written = write();
    Q_EMIT changed();
    return written;
}

bool WatchedOutbox::remove(const QString &serverId, const QString &userId, const QString &itemId)
{
    const QString key = serverId.toLower() + QLatin1Char('/') + userId.toLower() + QLatin1Char('/')
                        + itemId.toLower();
    for (int row = 0; row < m_events.size(); ++row) {
        if (m_events.at(row).key() != key)
            continue;
        m_events.removeAt(row);
        m_dirty = true;
        const bool written = write();
        Q_EMIT changed();
        return written;
    }
    return true;
}

void WatchedOutbox::flush()
{
    if (m_dirty)
        write();
}

bool WatchedOutbox::write()
{
    if (m_path.isEmpty()) {
        m_dirty = false;
        return true;
    }
    QJsonArray events;
    for (const WatchedEvent &event : std::as_const(m_events))
        events.append(eventObject(event));
    const QByteArray data = QJsonDocument(QJsonObject{{QStringLiteral("version"), FileVersion},
                                                      {QStringLiteral("events"), events}})
                                .toJson(QJsonDocument::Compact);
    QString error;
    if (!writePrivateFile(m_path, data, &error)) {
        if (!m_saveFailing)
            Q_EMIT saveFailed();
        m_saveFailing = true;
        return false;
    }
    m_saveFailing = false;
    m_dirty = false;
    return true;
}
} // namespace Kinefin
