// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QElapsedTimer>
#include <QJsonArray>
#include <QLoggingCategory>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>

#include <functional>

#include "api/JellyfinClient.h"

class QWebSocket;

Q_DECLARE_LOGGING_CATEGORY(socketLog)

namespace Kinefin {

// A remote `Play` command routed over the WebSocket. `command` is the validated `Data.Command` (the mode the coordinator honours), `itemIds` the validated 1..1000 ids, and `startPositionTicks` the optional server integer (-1 when the frame carried none)
struct RemotePlayRequest {
    QString command;
    QStringList itemIds;
    qint64 startPositionTicks = -1;
};

// One authenticated WebSocket per active identity (socket, event routing and missed-event wake-ups; the contract is websocket-contract.md). It owns no token: every attempt asks `JellyfinClient` for a fresh request, and it stops as soon as the client drops a credential. REST and the socket share the same server session; never parse playback state from frames, remote commands route to the coordinator, not to the reporter
class JellyfinSocketClient final : public QObject {
    Q_OBJECT

  public:
    enum class State { Disconnected, Connecting, Authenticated, Reconnecting };
    Q_ENUM(State)

    explicit JellyfinSocketClient(JellyfinClient *client, QObject *parent = nullptr);
    ~JellyfinSocketClient() override;

    State state() const;

    // Network back or an explicit user retry: the attempt counter resets and the next attempt opens at once, before the backoff timer. No-op unless the connection is currently between retries
    void reconnectNow();
    // Whether this identity wants `Sessions` broadcasts; remembered and (re)sent after every entry into Authenticated, withdrawn with SessionsStop. Admin sockets only (live check 9: the server sends no `Sessions` frames to a non-admin), so regular users poll `GET /Sessions` instead
    void setSessionsSubscription(bool subscribed);

    // The reconnect delay for `attempt` (ms) after `attempt` previous consecutive failures: ceiling `min(cap, base·2ⁿ)`, uniformly drawn in [ceiling/2, ceiling] by `unitRandom` in [0,1]. Pure and injectable for tests
    static qint64 reconnectDelay(int attempt, qint64 baseMs, qint64 capMs, double unitRandom);

    // Test-only: in ms, where a non-positive value keeps the production constant
    void setTestTimings(qint64 connectTimeoutMs, qint64 deadPeerMs, qint64 keepAliveIntervalMs,
                        qint64 backoffBaseMs, qint64 backoffCapMs);
    // Test-only: replaces the uniform draw of scheduleReconnect()
    void setRandomSource(std::function<double()> unitRandom);

  Q_SIGNALS:
    void stateChanged(Kinefin::JellyfinSocketClient::State state);
    // A library changed somewhere on the server: a wake-up only, consumers re-enumerate over REST
    void libraryChanged();
    void userDataChanged(const QStringList &itemIds);
    // `seekPositionTicks` is -1 whenever the frame carried none; the command is the validated 1..32-letter string
    void playstateCommand(const QString &command, qint64 seekPositionTicks);
    void generalCommand(const QString &name, const QVariantMap &arguments);
    void playRequested(const Kinefin::RemotePlayRequest &request);
    // Only while subscribed; the consumer re-fetches over REST, frames are never a complete delta
    void sessionsUpdated(const QJsonArray &sessions);

  private:
    void start();
    void stopForCredentialsLoss();
    void teardown();
    void beginAttempt();
    void onConnected(quint64 generation);
    void onDisconnected(quint64 generation);
    void onTextMessage(quint64 generation, const QString &text);
    void handleUnexpectedDisconnect(bool connectingPhase);
    void scheduleReconnect();
    void enterState(State next);
    void sendKeepAlive();
    void sendText(const QString &text);
    void restartDeadPeer();
    void dispatchForceKeepAlive(qint64 dataSeconds);

    QPointer<JellyfinClient> m_client;
    QWebSocket *m_socket = nullptr;
    quint64 m_generation = 0;
    State m_state = State::Disconnected;
    bool m_sessionsSubscribed = false;
    // One count for every frame a validation rule rejected; never their contents
    int m_droppedFrames = 0;
    int m_oversizeFrames = 0;

    QTimer m_connectTimer;
    QTimer m_deadPeerTimer;
    QTimer m_keepAliveTimer;
    QTimer m_reconnectTimer;
    QElapsedTimer m_uptime;
    int m_attempt = 0;
    std::function<double()> m_random;
    bool m_closing = false;

    qint64 m_connectTimeoutMs;
    qint64 m_deadPeerMs;
    qint64 m_keepAliveOverrideMs = 0;
    qint64 m_backoffBaseMs;
    qint64 m_backoffCapMs;
};

} // namespace Kinefin

Q_DECLARE_METATYPE(Kinefin::RemotePlayRequest)
Q_DECLARE_METATYPE(Kinefin::JellyfinSocketClient::State)
