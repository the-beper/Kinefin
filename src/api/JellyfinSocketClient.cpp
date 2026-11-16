// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "api/JellyfinSocketClient.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRandomGenerator>
#include <QSet>
#include <QWebSocket>
#include <QWebSocketProtocol>

#include <cmath>
#include <limits>

Q_LOGGING_CATEGORY(socketLog, "kinefin.api.socket")

namespace Kinefin {
namespace {

constexpr qint64 kConnectTimeoutMs = 15000;
constexpr qint64 kDeadPeerMs = 75000;
constexpr qint64 kKeepAliveCeilingMs = 30000;
constexpr qint64 kBackoffBaseMs = 1000;
constexpr qint64 kBackoffCapMs = 60000;
// A connection that lasted this long counts as stable, so its loss restarts the backoff ladder
constexpr qint64 kStableUptimeMs = 30000;
// Frame and message size limit, to revisit after a live library scan (websocket-contract.md)
constexpr qint64 kMaxFrameBytes = qint64(4) * 1024 * 1024;
constexpr int kMaxUserDataIds = 10000;
constexpr int kMaxSessionObjects = 200;
constexpr int kMaxArgumentKeys = 32;
constexpr int kMaxArgumentKeyChars = 64;
constexpr int kMaxArgumentValueChars = 8192;

const QString keepAliveMessage = QStringLiteral("{\"MessageType\":\"KeepAlive\"}");
// Data format inferred: "page,count" (websocket-contract.md, live check 9)
const QString sessionsStartMessage =
    QStringLiteral("{\"MessageType\":\"SessionsStart\",\"Data\":\"0,1500\"}");
const QString sessionsStopMessage = QStringLiteral("{\"MessageType\":\"SessionsStop\"}");

// The MessageType must be 1..64 plain letters; a frame that fails any check is dropped and counted, the connection stays open
bool lettersOnly(const QString &value, int maxChars)
{
    if (value.isEmpty() || value.size() > maxChars)
        return false;
    for (const QChar c : value) {
        const char16_t u = c.unicode();
        if (!((u >= u'a' && u <= u'z') || (u >= u'A' && u <= u'Z')))
            return false;
    }
    return true;
}

// JSON integers this component accepts: non-negative, integral and within the range two doubles cannot mangle. Absent or null values must be handled by the caller, never here
bool nonNegativeIntegral(const QJsonValue &value, qint64 *out)
{
    if (!value.isDouble())
        return false;
    const double number = value.toDouble();
    if (number < 0 || number > 9'007'199'254'740'991.0 || std::floor(number) != number)
        return false;
    if (out)
        *out = qint64(number);
    return true;
}

QString stateName(JellyfinSocketClient::State state)
{
    switch (state) {
    case JellyfinSocketClient::State::Disconnected:
        return QStringLiteral("Disconnected");
    case JellyfinSocketClient::State::Connecting:
        return QStringLiteral("Connecting");
    case JellyfinSocketClient::State::Authenticated:
        return QStringLiteral("Authenticated");
    case JellyfinSocketClient::State::Reconnecting:
        break;
    }
    return QStringLiteral("Reconnecting");
}

} // namespace

JellyfinSocketClient::JellyfinSocketClient(JellyfinClient *client, QObject *parent)
    : QObject(parent), m_client(client), m_connectTimeoutMs(kConnectTimeoutMs),
      m_deadPeerMs(kDeadPeerMs), m_backoffBaseMs(kBackoffBaseMs), m_backoffCapMs(kBackoffCapMs)
{
    m_random = [] { return QRandomGenerator::global()->generateDouble(); };
    m_connectTimer.setSingleShot(true);
    m_deadPeerTimer.setSingleShot(true);
    m_reconnectTimer.setSingleShot(true);
    connect(&m_connectTimer, &QTimer::timeout, this, [this] {
        if (m_state != State::Connecting)
            return;
        // Never leave a half-open attempt behind
        if (m_socket)
            m_socket->abort();
        handleUnexpectedDisconnect(true);
    });
    connect(&m_deadPeerTimer, &QTimer::timeout, this, [this] {
        if (m_state == State::Authenticated)
            handleUnexpectedDisconnect(false);
    });
    connect(&m_keepAliveTimer, &QTimer::timeout, this, [this] { sendKeepAlive(); });
    connect(&m_reconnectTimer, &QTimer::timeout, this, [this] { beginAttempt(); });
    if (m_client) {
        connect(m_client, &JellyfinClient::loginSucceeded, this, [this] { start(); });
        // Token loss covers a 401, a logout, a re-connect and profile switching alike
        connect(m_client, &JellyfinClient::credentialsCleared, this,
                [this] { stopForCredentialsLoss(); });
        connect(m_client, &QObject::destroyed, this, [this] { stopForCredentialsLoss(); });
    }
}

JellyfinSocketClient::~JellyfinSocketClient()
{
    teardown();
}

JellyfinSocketClient::State JellyfinSocketClient::state() const
{
    return m_state;
}

void JellyfinSocketClient::start()
{
    teardown();
    if (!m_client || !m_client->isAuthenticated()) {
        enterState(State::Disconnected);
        return;
    }
    m_attempt = 0;
    enterState(State::Connecting);
    beginAttempt();
}

void JellyfinSocketClient::reconnectNow()
{
    if (m_state != State::Reconnecting)
        return;
    m_attempt = 0;
    m_reconnectTimer.stop();
    beginAttempt();
}

void JellyfinSocketClient::stopForCredentialsLoss()
{
    teardown();
    enterState(State::Disconnected);
}

void JellyfinSocketClient::teardown()
{
    ++m_generation;
    m_connectTimer.stop();
    m_deadPeerTimer.stop();
    m_keepAliveTimer.stop();
    m_reconnectTimer.stop();
    m_uptime.invalidate();
    if (m_socket) {
        // One QWebSocket per attempt: the bump above already stale-dated every handler, the deleteLater is the teardown
        m_socket->disconnect(this);
        m_socket->close(QWebSocketProtocol::CloseCodeNormal);
        m_socket->deleteLater();
        m_socket = nullptr;
    }
}

void JellyfinSocketClient::beginAttempt()
{
    if (!m_client || !m_client->isAuthenticated()) {
        stopForCredentialsLoss();
        return;
    }
    m_droppedFrames = 0;
    m_oversizeFrames = 0;
    const quint64 generation = ++m_generation;
    m_socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    connect(m_socket, &QWebSocket::connected, this,
            [this, generation] { onConnected(generation); });
    connect(m_socket, &QWebSocket::disconnected, this,
            [this, generation] { onDisconnected(generation); });
    connect(m_socket, &QWebSocket::errorOccurred, this,
            [this, generation](QAbstractSocket::SocketError error) {
                if (generation != m_generation)
                    return;
                // The enum only, never errorString(): a localized message may carry paths or headers
                qCWarning(socketLog) << "socket error" << int(error);
            });
    connect(m_socket, &QWebSocket::textMessageReceived, this,
            [this, generation](const QString &text) { onTextMessage(generation, text); });
    connect(m_socket, &QWebSocket::binaryMessageReceived, this,
            [this, generation](const QByteArray &) {
                if (generation != m_generation)
                    return;
                // Text frames only: an unexpected non-text frame is dropped and counted, the connection stays open
                ++m_droppedFrames;
                qCWarning(socketLog) << "dropped binary frame" << m_droppedFrames;
                restartDeadPeer();
            });
    // A fresh attempt always asks the client for a current request, so a re-used token can never happen
    const QNetworkRequest request = m_client->webSocketRequest();
    if (request.url().isEmpty()) {
        stopForCredentialsLoss();
        return;
    }
    enterState(State::Connecting);
    m_connectTimer.start(qint32(m_connectTimeoutMs));
    m_socket->open(request);
}

void JellyfinSocketClient::onConnected(quint64 generation)
{
    if (generation != m_generation || !m_socket)
        return;
    m_connectTimer.stop();
    m_uptime.start();
    enterState(State::Authenticated);
    restartDeadPeer();
    if (m_sessionsSubscribed)
        sendText(sessionsStartMessage);
}

void JellyfinSocketClient::onDisconnected(quint64 generation)
{
    if (generation != m_generation)
        return;
    // The peer closes the connection; there is nothing left to close, but the socket object is ours to free
    if (m_socket) {
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    if (m_state == State::Authenticated)
        handleUnexpectedDisconnect(false);
    else if (m_state == State::Connecting || m_state == State::Reconnecting)
        handleUnexpectedDisconnect(true);
}

void JellyfinSocketClient::handleUnexpectedDisconnect(bool connectingPhase)
{
    // A stable connection resets the backoff ladder; nothing else does
    if (!connectingPhase && m_uptime.isValid() && m_uptime.elapsed() >= kStableUptimeMs)
        m_attempt = 0;
    ++m_generation;
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->close(QWebSocketProtocol::CloseCodeNormal);
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    m_connectTimer.stop();
    m_deadPeerTimer.stop();
    m_keepAliveTimer.stop();
    m_uptime.invalidate();
    if (connectingPhase && m_client)
        m_client->verifySession(); // silently expires a dead token, no-op otherwise
    enterState(State::Reconnecting);
    scheduleReconnect();
}

void JellyfinSocketClient::scheduleReconnect()
{
    const qint64 delay = reconnectDelay(m_attempt, m_backoffBaseMs, m_backoffCapMs, m_random());
    ++m_attempt;
    // Keep the single-shot timer type safe: the production delays never reach it
    m_reconnectTimer.start(delay <= qint64(std::numeric_limits<int>::max())
                               ? qint32(delay)
                               : std::numeric_limits<int>::max());
}

void JellyfinSocketClient::enterState(State next)
{
    if (next == m_state)
        return;
    m_state = next;
    qCInfo(socketLog) << "state" << stateName(m_state);
    if (auto *client = m_client.data()) {
        client->setMediaControlAnnounced(m_state == State::Authenticated);
        // Every entry into Authenticated announces the capabilities again (live check 8 re-checks)
        if (m_state == State::Authenticated)
            client->reportCapabilities();
    }
    Q_EMIT stateChanged(m_state);
}

void JellyfinSocketClient::restartDeadPeer()
{
    m_deadPeerTimer.start(m_deadPeerMs > qint64(std::numeric_limits<int>::max())
                              ? std::numeric_limits<int>::max()
                              : qint32(m_deadPeerMs));
}

void JellyfinSocketClient::sendText(const QString &text)
{
    if (m_socket && m_socket->isValid())
        m_socket->sendTextMessage(text);
}

void JellyfinSocketClient::sendKeepAlive()
{
    sendText(keepAliveMessage);
}

void JellyfinSocketClient::dispatchForceKeepAlive(qint64 dataSeconds)
{
    sendKeepAlive();
    const qint64 interval = m_keepAliveOverrideMs > 0
                                ? m_keepAliveOverrideMs
                                : qMin(dataSeconds * 500, kKeepAliveCeilingMs);
    // The heartbeat keeps the session alive; it states nothing about playback
    m_keepAliveTimer.start(interval <= qint64(std::numeric_limits<int>::max())
                               ? qint32(interval)
                               : std::numeric_limits<int>::max());
}

void JellyfinSocketClient::setSessionsSubscription(bool subscribed)
{
    if (m_sessionsSubscribed == subscribed)
        return;
    m_sessionsSubscribed = subscribed;
    if (m_state != State::Authenticated)
        return;
    sendText(subscribed ? sessionsStartMessage : sessionsStopMessage);
}

void JellyfinSocketClient::onTextMessage(quint64 generation, const QString &text)
{
    if (generation != m_generation)
        return;
    restartDeadPeer();
    const QByteArray bytes = text.toUtf8();
    if (bytes.size() > kMaxFrameBytes) {
        ++m_oversizeFrames;
        qCWarning(socketLog) << "oversize frame" << m_oversizeFrames;
        // MessageTooBig (1009); the resulting disconnected drives the reconnect ladder
        m_socket->close(QWebSocketProtocol::CloseCodeTooMuchData);
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        ++m_droppedFrames;
        qCWarning(socketLog) << "dropped malformed frame" << m_droppedFrames;
        return;
    }
    const QJsonObject envelope = document.object();
    const QJsonValue typeValue = envelope.value(QStringLiteral("MessageType"));
    if (!typeValue.isString()) {
        ++m_droppedFrames;
        qCWarning(socketLog) << "dropped frame without type" << m_droppedFrames;
        return;
    }
    // MessageId is ignored and nothing is deduplicated: the server never duplicates, so clients must not infer exactly-once delivery
    const QString type = typeValue.toString();
    if (!lettersOnly(type, 64)) {
        ++m_droppedFrames;
        qCWarning(socketLog) << "dropped frame with invalid type" << m_droppedFrames;
        return;
    }
    const QJsonValue dataValue = envelope.value(QStringLiteral("Data"));
    auto drop = [this](const char *rule) {
        ++m_droppedFrames;
        qCWarning(socketLog) << "dropped frame" << rule << m_droppedFrames;
    };

    if (type == QLatin1String("ForceKeepAlive") || type == QLatin1String("KeepAlive")) {
        if (type == QLatin1String("ForceKeepAlive")) {
            qint64 seconds = 60; // the observed default (websocket-contract.md)
            if (!nonNegativeIntegral(dataValue, &seconds) || seconds < 10 || seconds > 3600)
                seconds = 60;
            dispatchForceKeepAlive(seconds);
        }
        return;
    }
    if (type == QLatin1String("LibraryChanged")) {
        // A wake-up only: consumers re-enumerate from REST, `Data.Shape` stays unparsed
        if (!dataValue.isObject()) {
            drop("library-data");
            return;
        }
        Q_EMIT libraryChanged();
        return;
    }
    if (type == QLatin1String("UserDataChanged")) {
        if (!dataValue.isObject()) {
            drop("user-data-shape");
            return;
        }
        const QJsonObject data = dataValue.toObject();
        auto *client = m_client.data();
        if (!client || data.value(QStringLiteral("UserId")).toString() != client->userId()) {
            drop("user-data-identity");
            return;
        }
        // The live 12.4 shape carries the changes in Data.UserDataList (each entry with its own ItemId; the server rows include collection/parent entries, consumers fetch again). The older ItemsIdList guess stays accepted: both must satisfy the id budget
        QStringList result;
        QSet<QString> seen;
        const QJsonValue listValue = dataValue.toObject().value(QStringLiteral("UserDataList"));
        if (listValue.isArray()) {
            const QJsonArray rows = listValue.toArray();
            if (rows.size() > kMaxUserDataIds) {
                drop("user-data-ids");
                return;
            }
            for (const QJsonValue &rowValue : rows) {
                const QString idString =
                    rowValue.toObject().value(QStringLiteral("ItemId")).toString();
                if (!isValidJellyfinId(idString)) {
                    drop("user-data-ids");
                    return;
                }
                if (!seen.contains(idString)) {
                    seen.insert(idString);
                    result.append(idString);
                }
            }
        } else {
            const QJsonValue idsValue = data.value(QStringLiteral("ItemsIdList"));
            if (!idsValue.isArray() || idsValue.toArray().size() > kMaxUserDataIds) {
                drop("user-data-ids");
                return;
            }
            const QJsonArray ids = idsValue.toArray();
            for (const QJsonValue &id : ids) {
                if (!id.isString() || !isValidJellyfinId(id.toString())) {
                    drop("user-data-ids");
                    return;
                }
                // The server also sends parent items; the consumers fetch again, never trust the payload
                const QString idString = id.toString();
                if (!seen.contains(idString)) {
                    seen.insert(idString);
                    result.append(idString);
                }
            }
        }
        Q_EMIT userDataChanged(result);
        return;
    }
    if (type == QLatin1String("Playstate")) {
        if (!dataValue.isObject()) {
            drop("playstate-shape");
            return;
        }
        const QJsonObject data = dataValue.toObject();
        const QString command = data.value(QStringLiteral("Command")).toString();
        if (!lettersOnly(command, 32)) {
            drop("playstate-command");
            return;
        }
        const QJsonValue ticksValue = data.value(QStringLiteral("SeekPositionTicks"));
        qint64 ticks = -1;
        if (!ticksValue.isUndefined() && !ticksValue.isNull()
            && (!nonNegativeIntegral(ticksValue, &ticks))) {
            drop("playstate-ticks");
            return;
        }
        Q_EMIT playstateCommand(command, ticks);
        return;
    }
    if (type == QLatin1String("GeneralCommand")) {
        if (!dataValue.isObject()) {
            drop("general-command-shape");
            return;
        }
        const QJsonObject data = dataValue.toObject();
        const QString name = data.value(QStringLiteral("Name")).toString();
        if (!lettersOnly(name, 64)) {
            drop("general-command-name");
            return;
        }
        const QJsonValue argumentsValue = data.value(QStringLiteral("Arguments"));
        QVariantMap arguments;
        if (!argumentsValue.isUndefined()) {
            if (!argumentsValue.isObject()) {
                drop("general-command-arguments");
                return;
            }
            const QJsonObject raw = argumentsValue.toObject();
            if (raw.size() > kMaxArgumentKeys) {
                drop("general-command-arguments");
                return;
            }
            for (auto it = raw.begin(); it != raw.end(); ++it) {
                if (it.key().size() > kMaxArgumentKeyChars) {
                    drop("general-command-arguments");
                    return;
                }
                // Strings only: every remote value is text at the boundary, never QML or HTML
                if (!it.value().isString()
                    || it.value().toString().size() > kMaxArgumentValueChars) {
                    drop("general-command-arguments");
                    return;
                }
                arguments.insert(it.key(), it.value().toString());
            }
        }
        Q_EMIT generalCommand(name, arguments);
        return;
    }
    if (type == QLatin1String("Play")) {
        if (!dataValue.isObject()) {
            drop("play-shape");
            return;
        }
        const QJsonObject data = dataValue.toObject();
        // Observed on 12.4.0: Data {ItemIds, PlayCommand, ControllingUserId}; the legacy `Command` name is accepted too, the letter budget identical either way
        QString command = data.value(QStringLiteral("PlayCommand")).toString();
        if (command.isEmpty())
            command = data.value(QStringLiteral("Command")).toString();
        if (!lettersOnly(command, 32)) {
            drop("play-command");
            return;
        }
        const QJsonValue idsValue = data.value(QStringLiteral("ItemIds"));
        if (!idsValue.isArray()) {
            drop("play-ids");
            return;
        }
        const QJsonArray ids = idsValue.toArray();
        if (ids.isEmpty() || ids.size() > 1000) {
            drop("play-ids");
            return;
        }
        QStringList itemIds;
        for (const QJsonValue &id : ids) {
            if (!id.isString() || !isValidJellyfinId(id.toString())) {
                drop("play-ids");
                return;
            }
            itemIds.append(id.toString());
        }
        const QJsonValue ticksValue = data.value(QStringLiteral("StartPositionTicks"));
        qint64 ticks = -1;
        if (!ticksValue.isUndefined() && !ticksValue.isNull()
            && !nonNegativeIntegral(ticksValue, &ticks)) {
            drop("play-ticks");
            return;
        }
        const QJsonValue indexValue = data.value(QStringLiteral("StartIndex"));
        qint64 startIndex = -1;
        if (!indexValue.isUndefined() && !indexValue.isNull()
            && !nonNegativeIntegral(indexValue, &startIndex)) {
            drop("play-start-index");
            return;
        }
        if (startIndex >= itemIds.size())
            startIndex = -1;
        Q_EMIT playRequested(RemotePlayRequest{command, itemIds, ticks, int(startIndex)});
        return;
    }
    if (type == QLatin1String("Sessions")) {
        if (!m_sessionsSubscribed || !dataValue.isArray()) {
            drop("sessions-availability");
            return;
        }
        const QJsonArray sessions = dataValue.toArray();
        if (sessions.size() > kMaxSessionObjects) {
            drop("sessions-size");
            return;
        }
        for (const QJsonValue &session : sessions) {
            if (!session.isObject()) {
                drop("sessions-objects");
                return;
            }
        }
        Q_EMIT sessionsUpdated(sessions);
        return;
    }
    if (type == QLatin1String("UserDeleted")) {
        auto *client = m_client.data();
        // Our user was deleted: the session cannot be trusted any more
        if (client && dataValue.toString() == client->userId())
            client->verifySession();
        else
            drop("user-deleted-identity");
        return;
    }
    // ServerRestarting, ServerShuttingDown, RestartRequired, UserUpdated, SyncPlay*, *Timer*, RefreshProgress, ScheduledTask*, Package*, ActivityLog* and unknown names: ignored, never closing the connection
}

qint64 JellyfinSocketClient::reconnectDelay(int attempt, qint64 baseMs, qint64 capMs,
                                            double unitRandom)
{
    if (attempt < 0)
        attempt = 0;
    if (baseMs <= 0)
        return 0;
    qint64 ceiling = baseMs;
    for (int i = 0; i < attempt; ++i) {
        const qint64 doubled = ceiling * 2;
        if (doubled <= 0 || doubled > capMs) {
            ceiling = capMs;
            break;
        }
        ceiling = doubled;
    }
    if (ceiling > capMs)
        ceiling = capMs;
    const double draw =
        double(ceiling) / 2.0 + std::clamp(unitRandom, 0.0, 1.0) * double(ceiling) / 2.0;
    return qint64(draw);
}

void JellyfinSocketClient::setTestTimings(qint64 connectTimeoutMs, qint64 deadPeerMs,
                                          qint64 keepAliveIntervalMs, qint64 backoffBaseMs,
                                          qint64 backoffCapMs)
{
    if (connectTimeoutMs > 0)
        m_connectTimeoutMs = connectTimeoutMs;
    if (deadPeerMs > 0)
        m_deadPeerMs = deadPeerMs;
    if (keepAliveIntervalMs > 0)
        m_keepAliveOverrideMs = keepAliveIntervalMs;
    if (backoffBaseMs > 0)
        m_backoffBaseMs = backoffBaseMs;
    if (backoffCapMs > 0)
        m_backoffCapMs = backoffCapMs;
}

void JellyfinSocketClient::setRandomSource(std::function<double()> unitRandom)
{
    m_random = std::move(unitRandom);
}

} // namespace Kinefin
