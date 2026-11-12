// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/JellyfinClient.h"

#include <QAbstractListModel>
#include <QTimer>

#include <vector>

namespace Kinefin {
// The sessions the signed-in user may control. Polls only between open() and close() (the Remote Sessions dialog), never while a reload or a command is under way; never exposes raw JSON or accepts a session id supplied outside its snapshot. A failed refresh discards stale controls rather than letting them target a device whose permissions or playing state may have changed. A reload keeps the rows of the sessions that stay (updated in place, new ones appended, gone ones removed) so a view keeps its items and focus
class RemoteSessionsModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(Status status READ status NOTIFY statusChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(bool truncated READ truncated NOTIFY statusChanged)
    Q_PROPERTY(bool commandPending READ commandPending NOTIFY commandStateChanged)
    Q_PROPERTY(bool commandFailed READ commandFailed NOTIFY commandStateChanged)

  public:
    enum Status { Idle, Loading, Ready, Failed };
    Q_ENUM(Status)
    enum Role {
        SessionIdRole = Qt::UserRole + 1,
        DeviceNameRole,
        ClientNameRole,
        UserNameRole,
        NowPlayingRole,
        PlayingRole,
        PausedRole,
        CanControlRole
    };
    static constexpr int MaxSessions = 100;
    static constexpr int MaxTextChars = 128;
    static constexpr int PollIntervalMs = 5000;

    explicit RemoteSessionsModel(JellyfinClient *client, QObject *parent = nullptr);

    Q_INVOKABLE bool open();
    Q_INVOKABLE void reload();
    Q_INVOKABLE void close();
    // Only Pause, Unpause and Stop, only for a currently playing, controllable
    // session returned by the last successful GET /Sessions.
    Q_INVOKABLE bool control(const QString &sessionId, const QString &command);

    Status status() const { return m_status; }
    int count() const { return int(m_rows.size()); }
    bool truncated() const { return m_truncated; }
    bool commandPending() const { return !m_pendingSessionId.isEmpty(); }
    bool commandFailed() const { return m_commandFailed; }
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

  Q_SIGNALS:
    void statusChanged();
    void countChanged();
    void commandStateChanged();

  private:
    struct Session {
        QString id;
        QString device;
        QString client;
        QString user;
        QString title;
        bool playing = false;
        bool paused = false;
        bool controllable = false;
    };

    void setStatus(Status status);
    void clearRows();
    void applySessions(std::vector<Session> rows);
    void clear();
    void onSessionsReady(const QJsonArray &sessions);
    void onListingFailed(const QString &family, int status, const QString &reason, int channel);
    void onCommandFinished(const QString &sessionId, const QString &command, bool success);

    JellyfinClient *m_client = nullptr;
    std::vector<Session> m_rows;
    QTimer m_pollTimer;
    QString m_pendingSessionId;
    QString m_pendingCommand;
    Status m_status = Idle;
    bool m_open = false;
    bool m_truncated = false;
    bool m_commandFailed = false;
};
} // namespace Kinefin
