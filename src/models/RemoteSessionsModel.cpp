// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "RemoteSessionsModel.h"

#include <QJsonObject>
#include <QSet>

#include <algorithm>
#include <utility>

namespace Kinefin {
namespace {
QString plainLine(const QJsonValue &value)
{
    QString clean = value.toString().left(RemoteSessionsModel::MaxTextChars);
    for (QChar &character : clean) {
        const QChar::Category category = character.category();
        if (category == QChar::Other_Control || category == QChar::Other_Format
            || category == QChar::Separator_Line || category == QChar::Separator_Paragraph)
            character = QLatin1Char(' ');
    }
    return clean.simplified();
}
} // namespace

RemoteSessionsModel::RemoteSessionsModel(JellyfinClient *client, QObject *parent)
    : QAbstractListModel(parent), m_client(client)
{
    m_pollTimer.setInterval(PollIntervalMs);
    connect(&m_pollTimer, &QTimer::timeout, this, &RemoteSessionsModel::reload);
    if (!m_client)
        return;
    connect(m_client, &JellyfinClient::sessionsReady, this, &RemoteSessionsModel::onSessionsReady);
    connect(m_client, &JellyfinClient::listingFailed, this, &RemoteSessionsModel::onListingFailed);
    connect(m_client, &JellyfinClient::sessionPlaystateSucceeded, this,
            [this](const QString &id, const QString &command) {
                onCommandFinished(id, command, true);
            });
    connect(m_client, &JellyfinClient::sessionPlaystateFailed, this,
            [this](const QString &id, const QString &command, int) {
                onCommandFinished(id, command, false);
            });
    connect(m_client, &JellyfinClient::credentialsCleared, this, &RemoteSessionsModel::clear);
}

bool RemoteSessionsModel::open()
{
    if (!m_client || !m_client->isAuthenticated()) {
        clear();
        setStatus(Failed);
        return false;
    }
    if (m_open)
        return true;
    m_open = true;
    if (m_commandFailed) {
        m_commandFailed = false;
        Q_EMIT commandStateChanged();
    }
    m_pollTimer.start();
    reload();
    return true;
}

void RemoteSessionsModel::reload()
{
    if (!m_open || m_status == Loading)
        return;
    setStatus(Loading);
    if (!m_client || !m_client->fetchSessions(true)) {
        clearRows();
        setStatus(Failed);
    }
}

void RemoteSessionsModel::close()
{
    m_open = false;
    m_pollTimer.stop();
    clearRows();
    // A command already sent cannot be unsent. Keep it pending until its reply,
    // including across a quick close/reopen, so a duplicate callback cannot be
    // mistaken for the completion of a newly issued command.
    setStatus(Idle);
}

void RemoteSessionsModel::clear()
{
    close();
    if (commandPending() || m_commandFailed) {
        m_pendingSessionId.clear();
        m_pendingCommand.clear();
        m_commandFailed = false;
        Q_EMIT commandStateChanged();
    }
}

void RemoteSessionsModel::clearRows()
{
    const bool hadRows = !m_rows.empty();
    if (hadRows) {
        beginResetModel();
        m_rows.clear();
        endResetModel();
        Q_EMIT countChanged();
    }
    if (m_truncated) {
        m_truncated = false;
        Q_EMIT statusChanged();
    }
}

void RemoteSessionsModel::setStatus(Status status)
{
    if (m_status == status)
        return;
    m_status = status;
    Q_EMIT statusChanged();
}

void RemoteSessionsModel::onSessionsReady(const QJsonArray &sessions)
{
    if (!m_open || m_status != Loading)
        return;
    std::vector<Session> rows;
    rows.reserve(std::min(int(sessions.size()), MaxSessions));
    QSet<QString> ids;
    bool truncated = false;
    for (const QJsonValue &value : sessions) {
        const QJsonObject item = value.toObject();
        const QString id = item.value(QStringLiteral("Id")).toString();
        if (!isValidJellyfinId(id) || ids.contains(id.toLower()))
            continue;
        if (int(rows.size()) >= MaxSessions) {
            truncated = true;
            break;
        }
        ids.insert(id.toLower());
        const QJsonObject now = item.value(QStringLiteral("NowPlayingItem")).toObject();
        const QJsonObject playState = item.value(QStringLiteral("PlayState")).toObject();
        Session session;
        session.id = id;
        session.device = plainLine(item.value(QStringLiteral("DeviceName")));
        session.client = plainLine(item.value(QStringLiteral("Client")));
        session.user = plainLine(item.value(QStringLiteral("UserName")));
        session.title = plainLine(now.value(QStringLiteral("Name")));
        session.playing = !now.isEmpty() && isValidJellyfinId(now.value(QStringLiteral("Id")).toString());
        session.paused = session.playing && playState.value(QStringLiteral("IsPaused")).toBool();
        session.controllable = session.playing
                               && item.value(QStringLiteral("SupportsMediaControl")).toBool();
        rows.push_back(std::move(session));
    }
    beginResetModel();
    m_rows = std::move(rows);
    endResetModel();
    Q_EMIT countChanged();
    m_truncated = truncated;
    setStatus(Ready);
}

void RemoteSessionsModel::onListingFailed(const QString &family, int, const QString &, int channel)
{
    if (!m_open || m_status != Loading || channel != 0 || family != QLatin1String("Sessions"))
        return;
    clearRows();
    setStatus(Failed);
}

bool RemoteSessionsModel::control(const QString &sessionId, const QString &command)
{
    if (!m_open || m_status != Ready || commandPending()
        || (command != QLatin1String("Pause") && command != QLatin1String("Unpause")
            && command != QLatin1String("Stop")))
        return false;
    const auto it = std::find_if(m_rows.cbegin(), m_rows.cend(), [&sessionId](const Session &row) {
        return row.id == sessionId && row.controllable;
    });
    if (it == m_rows.cend() || (command == QLatin1String("Pause") && it->paused)
        || (command == QLatin1String("Unpause") && !it->paused))
        return false;
    if (!m_client || !m_client->sendSessionPlaystate(sessionId, command))
        return false;
    m_pendingSessionId = sessionId;
    m_pendingCommand = command;
    m_commandFailed = false;
    Q_EMIT commandStateChanged();
    return true;
}

void RemoteSessionsModel::onCommandFinished(const QString &id, const QString &command, bool success)
{
    if (m_pendingSessionId != id || m_pendingCommand != command)
        return;
    m_pendingSessionId.clear();
    m_pendingCommand.clear();
    m_commandFailed = !success;
    Q_EMIT commandStateChanged();
    if (success && m_open && m_status != Loading)
        reload();
}

int RemoteSessionsModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QVariant RemoteSessionsModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= int(m_rows.size()))
        return {};
    const Session &row = m_rows[size_t(index.row())];
    switch (role) {
    case SessionIdRole:
        return row.id;
    case DeviceNameRole:
        return row.device;
    case ClientNameRole:
        return row.client;
    case UserNameRole:
        return row.user;
    case NowPlayingRole:
        return row.title;
    case PlayingRole:
        return row.playing;
    case PausedRole:
        return row.paused;
    case CanControlRole:
        return row.controllable;
    }
    return {};
}

QHash<int, QByteArray> RemoteSessionsModel::roleNames() const
{
    return {{SessionIdRole, "sessionId"},  {DeviceNameRole, "deviceName"},
            {ClientNameRole, "clientName"}, {UserNameRole, "userName"},
            {NowPlayingRole, "nowPlaying"}, {PlayingRole, "playing"},
            {PausedRole, "paused"},         {CanControlRole, "canControl"}};
}
} // namespace Kinefin
