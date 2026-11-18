// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QList>
#include <QObject>
#include <QString>

#include <optional>

namespace Kinefin {
// One watched-state change made offline: the item was marked played and the server still has to learn it
struct WatchedEvent {
    QString serverId;
    QString userId;
    QString itemId;
    QString eventId;
    qint64 createdAt = 0;
    bool played = true;
    // The server's LastPlayedDate is newer than the local completion: the user decides
    bool conflict = false;

    bool operator==(const WatchedEvent &) const = default;
    // serverId/userId/itemId, lower case
    QString key() const;
};

// The durable outbox of one profile: `<data>/downloads/outbox.json`, versioned, atomic 0600, one event per item (a newer completion replaces an older one), at most `MaxEvents`. A file that is not JSON or is oversized is kept aside as `.broken`; a single invalid event is dropped alone
class WatchedOutbox final : public QObject {
    Q_OBJECT

  public:
    static constexpr int FileVersion = 1;
    static constexpr int MaxEvents = 1000;

    explicit WatchedOutbox(QObject *parent = nullptr);
    ~WatchedOutbox() override;

    void setStoragePath(const QString &path);
    QString storagePath() const { return m_path; }

    QList<WatchedEvent> events() const { return m_events; }
    std::optional<WatchedEvent> find(const QString &serverId, const QString &userId,
                                     const QString &itemId) const;
    // Inserts or replaces the event of its item; false for an invalid event or when the file cannot be written
    bool append(const WatchedEvent &event);
    bool remove(const QString &serverId, const QString &userId, const QString &itemId);
    void flush();

  Q_SIGNALS:
    void changed();
    void saveFailed();

  private:
    void load();
    bool write();

    QString m_path;
    QList<WatchedEvent> m_events;
    bool m_dirty = false;
    bool m_saveFailing = false;
};
} // namespace Kinefin
