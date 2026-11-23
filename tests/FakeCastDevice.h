// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "CastTestPki.h"

#include "cast/CastProto.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QSslConfiguration>
#include <QSslServer>
#include <QSslSocket>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QtEndian>

#include <optional>

// A scripted Cast device: a TLS server on loopback with the identity of a CastTestPki that cuts what it receives into CastMessages, records them with a timestamp, answers the challenge as `authMode` says and answers PINGs. Everything else is pushed by the test through the methods below, or, once `receiverEnabled` is set, answered like a Default Media Receiver host (see the members after `receiverEnabled`). One connection at a time is scripted: the latest. The state of the receiver (the app, the media session) belongs to the device and outlives a connection
class FakeCastDevice : public QObject {
  public:
    enum class AuthMode {
        // A valid response as soon as the challenge arrives
        Answer,
        // No answer until the test calls answerAuth()
        Manual,
        // An AuthError
        Error,
        // A valid response whose signature covers another certificate than the one of the TLS connection
        Tampered,
        // A valid response, then nothing: later frames are recorded and never answered, PINGs included
        AnswerThenSilent,
    };

    struct Received {
        Kinefin::CastMessage message;
        qint64 atMs = 0;
        // The connection the frame came on, 1 for the first
        int connection = 0;
    };

    explicit FakeCastDevice(const CastTestPki &pki, QObject *parent = nullptr)
        : QObject(parent), m_pki(&pki), m_connectionPki(&pki)
    {
        m_clock.start();
        const auto accept = [this] { acceptPending(); };
        // Which of the two signals a QSslServer emits for a connection whose handshake completed depends on the Qt version: the queue is drained by either, once
        connect(&m_server, &QTcpServer::newConnection, this, accept);
        connect(&m_server, &QTcpServer::pendingConnectionAvailable, this, accept);
    }

    bool listen() { return listen(QHostAddress::LocalHost); }

    bool listen(const QHostAddress &address)
    {
        applyIdentity();
        return m_server.listen(address, 0);
    }

    // The connections that follow are made with the TLS certificate and the leaf of `pki`; the one in progress keeps its own
    void setIdentity(const CastTestPki &pki)
    {
        m_pki = &pki;
        applyIdentity();
    }

    // No new connection is accepted; the one in progress goes on
    void stopListening() { m_server.close(); }

    quint16 port() const { return m_server.serverPort(); }

    static QJsonObject jsonOf(const Kinefin::CastMessage &message)
    {
        return QJsonDocument::fromJson(message.payload).object();
    }

    static QString typeOf(const Kinefin::CastMessage &message)
    {
        return jsonOf(message).value(QLatin1String("type")).toString();
    }

    int receivedCount() const { return static_cast<int>(received.size()); }

    // The recorded messages on `ns`, optionally of one JSON `type` and sent to one `destination`
    QList<Received> frames(const QString &ns, const QString &type = QString(),
                           const QString &destination = QString()) const
    {
        QList<Received> matching;
        for (const Received &entry : received) {
            if (entry.message.ns == ns && (type.isEmpty() || typeOf(entry.message) == type)
                && (destination.isEmpty() || entry.message.destinationId == destination))
                matching.append(entry);
        }
        return matching;
    }

    int count(const QString &ns, const QString &type = QString(),
              const QString &destination = QString()) const
    {
        return static_cast<int>(frames(ns, type, destination).size());
    }

    // The recorded messages of one connection (1 for the first), in the order they arrived
    QList<Received> onConnection(int connection) const
    {
        QList<Received> matching;
        for (const Received &entry : received) {
            if (entry.connection == connection)
                matching.append(entry);
        }
        return matching;
    }

    // The JSON of the latest LOAD received, empty when there was none
    QJsonObject lastLoad() const
    {
        for (qsizetype i = received.size() - 1; i >= 0; --i) {
            const Kinefin::CastMessage &message = received.at(i).message;
            if (message.ns == Kinefin::kCastNsMedia && typeOf(message) == QLatin1String("LOAD"))
                return jsonOf(message);
        }
        return QJsonObject();
    }

    // The response `authMode` asks for, to the latest challenge
    void answerAuth()
    {
        QByteArray payload;
        const CastTestPki &pki = m_connectionPki ? *m_connectionPki : *m_pki;
        if (authMode == AuthMode::Error) {
            payload = CastTestPki::authErrorMessage(0);
        } else {
            CastTestPki::AuthOptions options;
            options.signOverOtherPeer = authMode == AuthMode::Tampered;
            payload = pki.authResponseMessage(lastNonce, options);
        }
        pushBinary(QString(Kinefin::kCastPlatformId), QString(Kinefin::kCastNsDeviceAuth), payload);
        if (authMode == AuthMode::AnswerThenSilent)
            m_silent = true;
    }

    void pushText(const QString &source, const QString &ns, const QByteArray &payload)
    {
        sendMessage(messageFrom(source, ns, false, payload));
    }

    void push(const QString &source, const QString &ns, const QJsonObject &json)
    {
        pushText(source, ns, QJsonDocument(json).toJson(QJsonDocument::Compact));
    }

    void pushBinary(const QString &source, const QString &ns, const QByteArray &payload)
    {
        sendMessage(messageFrom(source, ns, true, payload));
    }

    void sendPing(const QString &source = QString(Kinefin::kCastPlatformId))
    {
        push(source, QString(Kinefin::kCastNsHeartbeat),
             QJsonObject{{QStringLiteral("type"), QStringLiteral("PING")}});
    }

    // `count` heartbeat frames in one write
    void flood(int count)
    {
        const QByteArray frame = Kinefin::frameCastMessage(
            messageFrom(QString(Kinefin::kCastPlatformId), QString(Kinefin::kCastNsHeartbeat),
                        false, QByteArrayLiteral(R"({"type":"PONG"})")));
        pushRaw(frame.repeated(count));
    }

    // Only the 4-byte big-endian length of a frame, as a device that announces a frame it never sends
    void sendLengthHeader(quint32 length)
    {
        QByteArray header(4, '\0');
        qToBigEndian(length, header.data());
        pushRaw(header);
    }

    void pushRaw(const QByteArray &bytes)
    {
        if (m_socket)
            m_socket->write(bytes);
    }

    void closeConnection()
    {
        if (m_socket)
            m_socket->disconnectFromHost();
    }

    // The connection ends at once with no goodbye, as a cable pulled or a device that reset
    void dropConnection()
    {
        if (m_socket)
            m_socket->abort();
    }

    AuthMode authMode = AuthMode::Answer;
    // The connections that completed the TLS handshake
    int connections = 0;
    // The frames received on the deviceauth namespace
    int challenges = 0;
    QByteArray lastNonce;
    QList<Received> received;

    // The receiver behaviour is off by default, so that the channel tests see the bare device. When on, CONNECT and CLOSE are kept per destination and a frame to a destination with no virtual connection is dropped and counted in `unconnectedFrames`
    bool receiverEnabled = false;
    int unconnectedFrames = 0;

    // The receiver namespace on receiver-0: GET_STATUS, LAUNCH, SET_VOLUME and an app STOP are answered with a RECEIVER_STATUS
    QString appId = QStringLiteral("CC1AD845");
    QString sessionId = QStringLiteral("session-1");
    QString transportId = QStringLiteral("transport-1");
    // False: LAUNCH is never answered
    bool answerLaunch = true;
    // The next `launchErrors` LAUNCH requests are answered with a LAUNCH_ERROR of `launchErrorReason`
    int launchErrors = 0;
    QString launchErrorReason = QStringLiteral("CANCELLED");
    // The answer to LAUNCH carries no transportId, a second status with it follows
    bool launchStatusWithoutTransport = false;
    double volumeLevel = 0.5;
    bool volumeMuted = false;
    QString volumeControlType = QStringLiteral("master");
    // The app of the receiver runs after LAUNCH and stops after an app STOP of its sessionId, which gives the ambient app back
    bool appRunning = false;
    bool ambientRunning = true;
    // The app STOPs of our sessionId received
    int appStops = 0;

    // The media namespace on the transport: LOAD answers IDLE (LOADING) with the LOAD's requestId, BUFFERING, PLAYING at 0 with the LOAD's requestId, then a spontaneous PLAYING at the start plus 0.3 s after `movingDelayMs`; `answerLoad` false means LOAD is never answered
    bool answerLoad = true;
    // LOAD is answered with LOAD_FAILED
    bool loadFails = false;
    // The next `ignoredLoads` LOADs are received and never answered, as a LOAD that a later one replaced
    int ignoredLoads = 0;
    int movingDelayMs = 40;
    // False: the spontaneous PLAYING with a moving position never comes (a stream the device cannot decode)
    bool movesAfterLoad = true;
    // False: PAUSE, PLAY, SEEK, STOP and GET_STATUS are never answered
    bool answerMedia = true;
    // A LOAD over a media session is followed by LOAD_CANCELLED for the request it replaced
    bool cancelOldLoadOnReload = false;
    int mediaSessionId = 0;
    QString contentId;
    QString playerState;
    bool hasMedia = false;

    // MEDIA_STATUS with `entry` as the only status, from the transport
    void pushMediaEntry(const QJsonObject &entry, qint64 requestId = 0)
    {
        pushMediaMessage(QJsonArray{entry}, requestId);
    }

    // A status entry of the media session `id` in the current position; `loading` adds extendedStatus LOADING, `withMedia` the media object with the contentId
    QJsonObject mediaEntry(qint64 id, const QString &state, const QString &reason = QString(),
                           bool loading = false, bool withMedia = false) const
    {
        QJsonObject entry{{QStringLiteral("mediaSessionId"), id},
                          {QStringLiteral("playerState"), state},
                          {QStringLiteral("currentTime"), positionSec()},
                          {QStringLiteral("playbackRate"), 1.0},
                          {QStringLiteral("supportedMediaCommands"), 274447}};
        if (!reason.isEmpty())
            entry.insert(QStringLiteral("idleReason"), reason);
        if (loading)
            entry.insert(QStringLiteral("extendedStatus"),
                         QJsonObject{{QStringLiteral("playerState"), QStringLiteral("LOADING")}});
        if (withMedia)
            entry.insert(QStringLiteral("media"),
                         QJsonObject{{QStringLiteral("contentId"), contentId},
                                     {QStringLiteral("streamType"), QStringLiteral("BUFFERED")}});
        return entry;
    }

    // `json` as it is, from the transport on the media namespace unless said otherwise: any frame a test wants the sender to read
    void sendStatus(const QJsonObject &json, const QString &ns = QString(Kinefin::kCastNsMedia),
                    const QString &source = QString())
    {
        push(source.isEmpty() ? transportId : source, ns, json);
    }

    // RECEIVER_STATUS as the device keeps it now (our app when it runs, the ambient app when idle)
    void pushReceiverStatus(qint64 requestId = 0, bool withTransport = true)
    {
        QJsonArray apps;
        if (appRunning) {
            QJsonObject app{
                {QStringLiteral("appId"), appId},
                {QStringLiteral("displayName"), QStringLiteral("Default Media Receiver")},
                {QStringLiteral("sessionId"), sessionId},
                {QStringLiteral("statusText"), QStringLiteral("Ready To Cast")}};
            if (withTransport)
                app.insert(QStringLiteral("transportId"), transportId);
            apps.append(app);
        }
        if (ambientRunning) {
            QJsonObject ambient{
                {QStringLiteral("appId"), QStringLiteral("E8C28D3C")},
                {QStringLiteral("displayName"), QStringLiteral("Backdrop")},
                {QStringLiteral("sessionId"), QStringLiteral("ambient-session")},
                {QStringLiteral("transportId"), QStringLiteral("ambient-transport")}};
            apps.append(ambient);
        }
        QJsonObject status{{QStringLiteral("volume"),
                            QJsonObject{{QStringLiteral("level"), volumeLevel},
                                        {QStringLiteral("muted"), volumeMuted},
                                        {QStringLiteral("controlType"), volumeControlType}}}};
        if (!apps.isEmpty())
            status.insert(QStringLiteral("applications"), apps);
        QJsonObject message = typedObject(QStringLiteral("RECEIVER_STATUS"), requestId);
        message.insert(QStringLiteral("status"), status);
        push(QString(Kinefin::kCastPlatformId), QString(Kinefin::kCastNsReceiver), message);
    }

    // Another sender launched its own app: a RECEIVER_STATUS that lists only that
    void takeOver()
    {
        appRunning = false;
        hasMedia = false;
        QJsonObject other{{QStringLiteral("appId"), QStringLiteral("233637DE")},
                          {QStringLiteral("displayName"), QStringLiteral("Another app")},
                          {QStringLiteral("sessionId"), QStringLiteral("foreign-session")},
                          {QStringLiteral("transportId"), QStringLiteral("foreign-transport")}};
        QJsonObject message = typedObject(QStringLiteral("RECEIVER_STATUS"), 0);
        message.insert(QStringLiteral("status"),
                       QJsonObject{{QStringLiteral("applications"), QJsonArray{other}}});
        push(QString(Kinefin::kCastPlatformId), QString(Kinefin::kCastNsReceiver), message);
    }

    // The app's transport says goodbye, as when the app ends on the device
    void closeTransport()
    {
        m_open.remove(transportId);
        push(transportId, QString(Kinefin::kCastNsConnection),
             QJsonObject{{QStringLiteral("type"), QStringLiteral("CLOSE")}});
    }

    // Whether the sender sent CONNECT to `destination` on the current connection and not CLOSE since
    bool isConnected(const QString &destination) const { return m_open.contains(destination); }

  private:
    static Kinefin::CastMessage messageFrom(const QString &source, const QString &ns, bool binary,
                                            const QByteArray &payload)
    {
        Kinefin::CastMessage message;
        message.sourceId = source;
        message.destinationId = QString(Kinefin::kCastSenderId);
        message.ns = ns;
        message.binary = binary;
        message.payload = payload;
        return message;
    }

    static QJsonObject typedObject(const QString &type, qint64 requestId)
    {
        return QJsonObject{{QStringLiteral("type"), type},
                           {QStringLiteral("requestId"), requestId}};
    }

    void sendMessage(const Kinefin::CastMessage &message)
    {
        pushRaw(Kinefin::frameCastMessage(message));
    }

    void applyIdentity()
    {
        QSslConfiguration configuration = QSslConfiguration::defaultConfiguration();
        configuration.setLocalCertificate(m_pki->tlsCertificate());
        configuration.setPrivateKey(m_pki->tlsKey());
        configuration.setPeerVerifyMode(QSslSocket::VerifyNone);
        m_server.setSslConfiguration(configuration);
    }

    void acceptPending()
    {
        while (QTcpSocket *socket = m_server.nextPendingConnection()) {
            ++connections;
            m_socket = socket;
            m_connectionPki = m_pki;
            m_framer = Kinefin::CastFramer();
            m_silent = false;
            m_open.clear();
            connect(socket, &QTcpSocket::readyRead, this, [this, socket] { drain(socket); });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            // Bytes that came with the handshake raised readyRead before it was connected
            if (socket->bytesAvailable() > 0)
                drain(socket);
        }
    }

    void drain(QTcpSocket *socket)
    {
        m_framer.append(socket->readAll());
        while (const std::optional<QByteArray> body = m_framer.next()) {
            const std::optional<Kinefin::CastMessage> message = Kinefin::decodeCastMessage(*body);
            if (!message)
                continue;
            received.append(Received{*message, m_clock.elapsed(), connections});
            react(*message);
        }
    }

    void react(const Kinefin::CastMessage &message)
    {
        if (m_silent)
            return;
        if (message.ns == Kinefin::kCastNsDeviceAuth) {
            ++challenges;
            // The payload is 0a 14 12 10 <16 bytes of nonce> 18 01
            lastNonce = message.payload.mid(4, 16);
            if (authMode != AuthMode::Manual)
                answerAuth();
            return;
        }
        if (message.ns == Kinefin::kCastNsHeartbeat) {
            if (typeOf(message) == QLatin1String("PING"))
                push(message.destinationId, QString(Kinefin::kCastNsHeartbeat),
                     QJsonObject{{QStringLiteral("type"), QStringLiteral("PONG")}});
            return;
        }
        if (receiverEnabled)
            reactAsReceiver(message);
    }

    void reactAsReceiver(const Kinefin::CastMessage &message)
    {
        const QJsonObject json = jsonOf(message);
        const QString type = json.value(QLatin1String("type")).toString();
        const qint64 requestId =
            static_cast<qint64>(json.value(QLatin1String("requestId")).toDouble(0));
        if (message.ns == Kinefin::kCastNsConnection) {
            if (type == QLatin1String("CONNECT"))
                m_open.insert(message.destinationId);
            else if (type == QLatin1String("CLOSE"))
                m_open.remove(message.destinationId);
            return;
        }
        if (!m_open.contains(message.destinationId)) {
            ++unconnectedFrames;
            return;
        }
        if (message.ns == Kinefin::kCastNsReceiver
            && message.destinationId == Kinefin::kCastPlatformId)
            reactToReceiver(type, requestId, json);
        else if (message.ns == Kinefin::kCastNsMedia && message.destinationId == transportId
                 && appRunning)
            reactToMedia(type, requestId, json);
    }

    void reactToReceiver(const QString &type, qint64 requestId, const QJsonObject &json)
    {
        if (type == QLatin1String("GET_STATUS")) {
            pushReceiverStatus(requestId);
        } else if (type == QLatin1String("LAUNCH")) {
            if (!answerLaunch)
                return;
            if (launchErrors > 0) {
                --launchErrors;
                QJsonObject error = typedObject(QStringLiteral("LAUNCH_ERROR"), requestId);
                error.insert(QStringLiteral("reason"), launchErrorReason);
                push(QString(Kinefin::kCastPlatformId), QString(Kinefin::kCastNsReceiver), error);
                return;
            }
            appRunning = true;
            ambientRunning = false;
            if (launchStatusWithoutTransport) {
                pushReceiverStatus(requestId, false);
                QTimer::singleShot(40, this, [this] { pushReceiverStatus(0); });
            } else {
                pushReceiverStatus(requestId);
            }
        } else if (type == QLatin1String("SET_VOLUME")) {
            const QJsonObject volume = json.value(QLatin1String("volume")).toObject();
            if (volume.contains(QLatin1String("level")))
                volumeLevel = volume.value(QLatin1String("level")).toDouble();
            if (volume.contains(QLatin1String("muted")))
                volumeMuted = volume.value(QLatin1String("muted")).toBool();
            pushReceiverStatus(requestId);
        } else if (type == QLatin1String("STOP")) {
            if (!appRunning || json.value(QLatin1String("sessionId")).toString() != sessionId)
                return;
            ++appStops;
            appRunning = false;
            ambientRunning = false;
            hasMedia = false;
            pushReceiverStatus(requestId);
            // What the device did when our app stopped: the transport goes, then the ambient app comes back
            closeTransport();
            ambientRunning = true;
            pushReceiverStatus(0);
        }
    }

    void reactToMedia(const QString &type, qint64 requestId, const QJsonObject &json)
    {
        if (type == QLatin1String("LOAD")) {
            handleLoad(requestId, json);
            return;
        }
        if (!answerMedia)
            return;
        if (type == QLatin1String("GET_STATUS")) {
            QJsonArray entries;
            if (hasMedia)
                entries.append(mediaEntry(mediaSessionId, playerState, QString(), false, true));
            pushMediaMessage(entries, requestId);
            return;
        }
        const qint64 target =
            static_cast<qint64>(json.value(QLatin1String("mediaSessionId")).toDouble(-1));
        if (!hasMedia || target != mediaSessionId) {
            QJsonObject invalid = typedObject(QStringLiteral("INVALID_REQUEST"), requestId);
            invalid.insert(QStringLiteral("reason"), QStringLiteral("INVALID_MEDIA_SESSION_ID"));
            push(transportId, QString(Kinefin::kCastNsMedia), invalid);
            return;
        }
        if (type == QLatin1String("PAUSE")) {
            freezeAt(positionSec());
            playerState = QStringLiteral("PAUSED");
            pushMediaEntry(mediaEntry(mediaSessionId, playerState), requestId);
        } else if (type == QLatin1String("PLAY")) {
            freezeAt(positionSec());
            playerState = QStringLiteral("PLAYING");
            pushMediaEntry(mediaEntry(mediaSessionId, playerState), requestId);
        } else if (type == QLatin1String("SEEK")) {
            freezeAt(json.value(QLatin1String("currentTime")).toDouble());
            m_moving = true;
            playerState = QStringLiteral("PLAYING");
            pushMediaEntry(mediaEntry(mediaSessionId, playerState), requestId);
        } else if (type == QLatin1String("STOP")) {
            playerState = QStringLiteral("IDLE");
            pushMediaEntry(mediaEntry(mediaSessionId, playerState, QStringLiteral("CANCELLED")),
                           requestId);
            hasMedia = false;
        }
    }

    void handleLoad(qint64 requestId, const QJsonObject &json)
    {
        if (!answerLoad)
            return;
        if (ignoredLoads > 0) {
            --ignoredLoads;
            return;
        }
        if (loadFails) {
            QJsonObject failure = typedObject(QStringLiteral("LOAD_FAILED"), requestId);
            failure.insert(QStringLiteral("detailedErrorCode"), 104);
            push(transportId, QString(Kinefin::kCastNsMedia), failure);
            return;
        }
        const qint64 previous = hasMedia ? mediaSessionId : 0;
        const qint64 previousRequest = m_lastLoadRequest;
        m_lastLoadRequest = requestId;
        const double start = json.value(QLatin1String("currentTime")).toDouble();
        mediaSessionId = ++m_mediaSessionCounter;
        contentId = json.value(QLatin1String("media"))
                        .toObject()
                        .value(QLatin1String("contentId"))
                        .toString();
        hasMedia = true;
        m_moving = false;
        freezeAt(0);
        playerState = QStringLiteral("IDLE");
        const int serial = ++m_loadSerial;
        if (previous != 0) {
            // As the device did for an own re-LOAD: the old session is interrupted, the new one starts with BUFFERING
            pushMediaEntry(
                mediaEntry(previous, QStringLiteral("IDLE"), QStringLiteral("INTERRUPTED"), true),
                0);
            if (cancelOldLoadOnReload) {
                QJsonObject cancelled =
                    typedObject(QStringLiteral("LOAD_CANCELLED"), previousRequest);
                push(transportId, QString(Kinefin::kCastNsMedia), cancelled);
            }
        } else {
            pushMediaEntry(mediaEntry(mediaSessionId, playerState, QString(), true, true),
                           requestId);
        }
        playerState = QStringLiteral("BUFFERING");
        pushMediaEntry(mediaEntry(mediaSessionId, playerState), 0);
        playerState = QStringLiteral("PLAYING");
        pushMediaEntry(mediaEntry(mediaSessionId, playerState), requestId);
        if (!movesAfterLoad)
            return;
        QTimer::singleShot(movingDelayMs, this, [this, serial, start] {
            if (serial != m_loadSerial || !hasMedia || playerState != QLatin1String("PLAYING"))
                return;
            freezeAt(start + 0.3);
            m_moving = true;
            pushMediaEntry(mediaEntry(mediaSessionId, playerState), 0);
        });
    }

    void pushMediaMessage(const QJsonArray &entries, qint64 requestId)
    {
        QJsonObject message = typedObject(QStringLiteral("MEDIA_STATUS"), requestId);
        message.insert(QStringLiteral("status"), entries);
        push(transportId, QString(Kinefin::kCastNsMedia), message);
    }

    void freezeAt(double seconds)
    {
        m_positionBase = seconds;
        m_positionClock.restart();
    }

    double positionSec() const
    {
        if (m_moving && playerState == QLatin1String("PLAYING") && m_positionClock.isValid())
            return m_positionBase + static_cast<double>(m_positionClock.elapsed()) / 1000.0;
        return m_positionBase;
    }

    const CastTestPki *m_pki;
    const CastTestPki *m_connectionPki;
    QSslServer m_server;
    QPointer<QTcpSocket> m_socket;
    Kinefin::CastFramer m_framer;
    QElapsedTimer m_clock;
    bool m_silent = false;
    QSet<QString> m_open;
    QElapsedTimer m_positionClock;
    double m_positionBase = 0;
    bool m_moving = false;
    qint64 m_lastLoadRequest = 0;
    int m_mediaSessionCounter = 0;
    int m_loadSerial = 0;
};
