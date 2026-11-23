// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cast/CastDeviceAuth.h"
#include "cast/CastProto.h"

#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QSslSocket>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <deque>

namespace Kinefin {

// One TLS connection to the port of a Cast device: the framing, the device authentication that must pass before anything else is sent, the virtual connections, the heartbeat, the requestId bookkeeping and the caps on what the device sends. The TLS check of the device's self-signed certificate is off for this socket only (device authentication replaces it); no other QSslConfiguration of the process is touched
class CastChannel final : public QObject {
    Q_OBJECT

  public:
    enum class State {
        Idle,
        Connecting,
        Authenticating,
        Open,
        Closed,
        Failed,
    };
    Q_ENUM(State)

    enum class FailReason {
        // The TCP connection or the TLS handshake did not complete in time or errored
        ConnectFailed,
        // The device did not answer the challenge in time
        AuthTimeout,
        // The device failed the authentication; the detail of failed() names the CastAuthResult
        AuthRefused,
        // Nothing arrived for livenessMs, or the socket closed or errored while open
        Lost,
        // A frame or a message that does not parse, or a frame over the cap
        Protocol,
        // More than floodFrames frames within floodWindowMs
        Flood,
    };
    Q_ENUM(FailReason)

    // The defaults are the production values; the tests shorten the times
    struct Options {
        CastTrust trust = CastTrust::google();
        bool requireVideo = true;
        int connectTimeoutMs = 10'000;
        int authTimeoutMs = 10'000;
        int pingIntervalMs = 5000;
        int livenessMs = 10'000;
        int requestTimeoutMs = 10'000;
        int maxPendingRequests = 16;
        int floodFrames = 256;
        int floodWindowMs = 5000;
    };

    CastChannel(const QHostAddress &address, quint16 port, const Options &options,
                QObject *parent = nullptr);
    CastChannel(const QHostAddress &address, quint16 port, QObject *parent = nullptr);
    ~CastChannel() override;

    State state() const { return m_state; }
    // SHA-256 of the DER of the client_auth_certificate that passed the device authentication: empty until the channel was Open. A reconnect compares it with the first one to know that it is the same device
    QByteArray authenticatedLeafHash() const;
    // Connects, runs TLS, sends the AuthChallenge as the first frame and waits for the device to pass: opened() follows. Anything the device sends before its answer is dropped and counts toward the flood cap. Only from Idle
    void open();
    // CLOSE on every virtual connection that was opened, then the socket: Closed, no failed()
    void close();

    // CONNECT on `destination` (receiver-0 or an app's transportId): its heartbeat runs from now on. Only while Open
    void connectTo(const QString &destination);
    // CLOSE on `destination` and no more heartbeat to it
    void closeTo(const QString &destination);
    // JSON with a requestId on `namespace`: returns that id, which the reply carries, or 0 when the channel is not Open or maxPendingRequests are waiting. A reply with the id ends the wait; after `timeoutMs` (0: Options::requestTimeoutMs) requestTimedOut() follows once
    quint32 request(const QString &destination, const QString &ns, QJsonObject body,
                    int timeoutMs = 0);
    // JSON without a requestId. False when the channel is not Open
    bool send(const QString &destination, const QString &ns, const QJsonObject &body);

  Q_SIGNALS:
    void stateChanged(Kinefin::CastChannel::State state);
    // The device passed the authentication
    void opened();
    // The channel is Failed and the socket closed. `detail` never holds a certificate, a nonce or a payload
    void failed(Kinefin::CastChannel::FailReason reason, const QString &detail);
    // A STRING payload that is a JSON object, on any namespace but the heartbeat; `requestId` of a reply is in the object. PING is answered by the channel itself
    void messageReceived(const QString &source, const QString &ns, const QJsonObject &json);
    void requestTimedOut(quint32 requestId);
    // The device sent CLOSE on the virtual connection to `source`
    void virtualConnectionClosed(const QString &source);

  private:
    struct Pending {
        quint32 id = 0;
        qint64 deadlineMs = 0;
    };

    void setState(State state);
    void fail(FailReason reason, const QString &detail);
    void onEncrypted();
    void onReadyRead();
    void onSocketClosed();
    void handleFrame(const QByteArray &body);
    void handleAuth(const CastMessage &message);
    void handleHeartbeatAndConnection(const CastMessage &message, const QJsonObject &json);
    bool countFrame();
    void tick();
    // False when the frame cannot be built (an id or the payload over its cap) or the socket is not connected
    bool sendFrame(const CastMessage &message);
    void teardown();

    QHostAddress m_address;
    quint16 m_port;
    Options m_options;
    QSslSocket m_socket;
    CastFramer m_framer;
    QTimer m_ticker;
    QTimer m_stageTimer;
    QElapsedTimer m_clock;
    State m_state = State::Idle;
    QByteArray m_nonce;
    QByteArray m_leafHash;
    QStringList m_virtualConnections;
    std::deque<Pending> m_pending;
    std::deque<qint64> m_frameTimes;
    quint32 m_nextRequestId = 1;
    qint64 m_lastFrameMs = 0;
    qint64 m_lastPingMs = 0;
};

} // namespace Kinefin
