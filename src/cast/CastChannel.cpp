// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "cast/CastChannel.h"

#include <QAnyStringView>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonValue>
#include <QLoggingCategory>
#include <QMetaEnum>
#include <QNetworkProxy>
#include <QRandomGenerator>
#include <QSslConfiguration>

#include <algorithm>
#include <cmath>
#include <limits>

namespace Kinefin {
namespace {
Q_LOGGING_CATEGORY(channelLog, "kinefin.cast.channel")

// Four words are the 16-byte nonce of the challenge
constexpr int kNonceWords = 4;
// Read in pieces and cut frames between them, so a burst never sits in the framer as one block
constexpr qint64 kReadChunkBytes = 16384;

const QByteArray kPingPayload = QByteArrayLiteral(R"({"type":"PING"})");
const QByteArray kPongPayload = QByteArrayLiteral(R"({"type":"PONG"})");
const QByteArray kConnectPayload = QByteArrayLiteral(R"({"type":"CONNECT","userAgent":"Kinefin"})");
const QByteArray kClosePayload = QByteArrayLiteral(R"({"type":"CLOSE"})");

QString authResultName(CastAuthResult result)
{
    switch (result) {
    case CastAuthResult::Ok:
        return QStringLiteral("Ok");
    case CastAuthResult::NoPeerCertificate:
        return QStringLiteral("NoPeerCertificate");
    case CastAuthResult::NoTrust:
        return QStringLiteral("NoTrust");
    case CastAuthResult::MalformedResponse:
        return QStringLiteral("MalformedResponse");
    case CastAuthResult::DeviceError:
        return QStringLiteral("DeviceError");
    case CastAuthResult::UnsupportedAlgorithm:
        return QStringLiteral("UnsupportedAlgorithm");
    case CastAuthResult::TooLarge:
        return QStringLiteral("TooLarge");
    case CastAuthResult::BadChain:
        return QStringLiteral("BadChain");
    case CastAuthResult::BadSignature:
        return QStringLiteral("BadSignature");
    case CastAuthResult::WeakKey:
        return QStringLiteral("WeakKey");
    case CastAuthResult::AudioOnly:
        return QStringLiteral("AudioOnly");
    }
    return QStringLiteral("Unknown");
}

// The enum name only: errorString() may carry the peer address
QString socketDetail(QAbstractSocket::SocketError error)
{
    if (error == QAbstractSocket::UnknownSocketError)
        return QStringLiteral("closed");
    return QString::fromLatin1(
        QMetaEnum::fromType<QAbstractSocket::SocketError>().valueToKey(static_cast<int>(error)));
}

CastMessage textMessage(const QString &destination, QAnyStringView ns, const QByteArray &payload)
{
    CastMessage message;
    message.sourceId = QString(kCastSenderId);
    message.destinationId = destination;
    message.ns = ns.toString();
    message.payload = payload;
    return message;
}

QByteArray compactJson(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

std::optional<QJsonObject> parseObject(const CastMessage &message)
{
    if (message.binary)
        return std::nullopt;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(message.payload, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return std::nullopt;
    return document.object();
}

// The requestId of a reply, 0 when there is none, it is not an integer or it is 0 (a spontaneous status)
quint32 requestIdOf(const QJsonObject &json)
{
    const QJsonValue value = json.value(QLatin1String("requestId"));
    if (!value.isDouble())
        return 0;
    const double number = value.toDouble();
    if (number < 1 || number > std::numeric_limits<quint32>::max() || number != std::floor(number))
        return 0;
    return static_cast<quint32>(number);
}
} // namespace

CastChannel::CastChannel(const QHostAddress &address, quint16 port, const Options &options,
                         QObject *parent)
    : QObject(parent), m_address(address), m_port(port), m_options(options)
{
    QSslConfiguration configuration = QSslConfiguration::defaultConfiguration();
    // A Cast device presents a self-signed certificate: device authentication replaces the chain check, for this socket only
    configuration.setPeerVerifyMode(QSslSocket::VerifyNone);
    configuration.setProtocol(QSsl::TlsV1_2OrLater);
    m_socket.setSslConfiguration(configuration);
    m_socket.setProxy(QNetworkProxy::NoProxy);
    connect(&m_socket, &QSslSocket::sslErrors, &m_socket,
            QOverload<>::of(&QSslSocket::ignoreSslErrors));
    connect(&m_socket, &QSslSocket::encrypted, this, &CastChannel::onEncrypted);
    connect(&m_socket, &QSslSocket::readyRead, this, &CastChannel::onReadyRead);
    connect(&m_socket, &QSslSocket::disconnected, this, &CastChannel::onSocketClosed);
    connect(&m_socket, &QSslSocket::errorOccurred, this, &CastChannel::onSocketClosed);

    m_ticker.setInterval(
        std::clamp(std::min(m_options.pingIntervalMs, m_options.livenessMs) / 4, 10, 100));
    connect(&m_ticker, &QTimer::timeout, this, &CastChannel::tick);

    m_stageTimer.setSingleShot(true);
    connect(&m_stageTimer, &QTimer::timeout, this, [this] {
        if (m_state == State::Connecting)
            fail(FailReason::ConnectFailed, QStringLiteral("timeout"));
        else if (m_state == State::Authenticating)
            fail(FailReason::AuthTimeout, QStringLiteral("timeout"));
    });
}

CastChannel::CastChannel(const QHostAddress &address, quint16 port, QObject *parent)
    : CastChannel(address, port, Options(), parent)
{
}

CastChannel::~CastChannel()
{
    // No signal may reach this object while its members are being destroyed
    QObject::disconnect(&m_socket, nullptr, this, nullptr);
    m_ticker.stop();
    m_stageTimer.stop();
    m_socket.abort();
}

QByteArray CastChannel::authenticatedLeafHash() const
{
    return m_leafHash;
}

void CastChannel::open()
{
    if (m_state != State::Idle)
        return;
    m_clock.start();
    m_lastFrameMs = 0;
    m_lastPingMs = 0;
    setState(State::Connecting);
    m_stageTimer.start(m_options.connectTimeoutMs);
    // Queued: the socket can report a refused connection from inside the call, and failed() must not reach the caller of open() before it returns
    QTimer::singleShot(0, this, [this] {
        // The address as text: with the check of the certificate off there is no host name to verify
        if (m_state == State::Connecting)
            m_socket.connectToHostEncrypted(m_address.toString(), m_port);
    });
}

void CastChannel::close()
{
    if (m_state == State::Closed || m_state == State::Failed)
        return;
    const bool wasOpen = m_state == State::Open;
    if (wasOpen) {
        for (const QString &destination : std::as_const(m_virtualConnections))
            sendFrame(textMessage(destination, kCastNsConnection, kClosePayload));
        m_socket.flush();
    }
    teardown();
    setState(State::Closed);
    if (wasOpen)
        m_socket.disconnectFromHost();
    else
        m_socket.abort();
}

void CastChannel::connectTo(const QString &destination)
{
    if (m_state != State::Open || m_virtualConnections.contains(destination))
        return;
    if (sendFrame(textMessage(destination, kCastNsConnection, kConnectPayload)))
        m_virtualConnections.append(destination);
}

void CastChannel::closeTo(const QString &destination)
{
    if (m_state != State::Open || m_virtualConnections.removeAll(destination) == 0)
        return;
    sendFrame(textMessage(destination, kCastNsConnection, kClosePayload));
}

quint32 CastChannel::request(const QString &destination, const QString &ns, QJsonObject body,
                             int timeoutMs)
{
    if (m_state != State::Open
        || static_cast<int>(m_pending.size()) >= m_options.maxPendingRequests)
        return 0;
    const quint32 id = m_nextRequestId;
    body.insert(QStringLiteral("requestId"), static_cast<qint64>(id));
    if (!sendFrame(textMessage(destination, ns, compactJson(body))))
        return 0;
    ++m_nextRequestId;
    const int timeout = timeoutMs > 0 ? timeoutMs : m_options.requestTimeoutMs;
    m_pending.push_back({id, m_clock.elapsed() + timeout});
    return id;
}

bool CastChannel::send(const QString &destination, const QString &ns, const QJsonObject &body)
{
    if (m_state != State::Open)
        return false;
    return sendFrame(textMessage(destination, ns, compactJson(body)));
}

void CastChannel::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    Q_EMIT stateChanged(state);
}

void CastChannel::fail(FailReason reason, const QString &detail)
{
    if (m_state == State::Failed || m_state == State::Closed)
        return;
    qCWarning(channelLog) << "channel failed:"
                          << QMetaEnum::fromType<FailReason>().valueToKey(static_cast<int>(reason))
                          << detail;
    teardown();
    // Before the abort, which emits disconnected()
    setState(State::Failed);
    m_socket.abort();
    Q_EMIT failed(reason, detail);
}

void CastChannel::teardown()
{
    m_ticker.stop();
    m_stageTimer.stop();
    m_pending.clear();
    m_frameTimes.clear();
    m_virtualConnections.clear();
}

void CastChannel::onEncrypted()
{
    if (m_state != State::Connecting)
        return;
    setState(State::Authenticating);
    if (m_state != State::Authenticating)
        return;
    m_stageTimer.start(m_options.authTimeoutMs);
    if (m_socket.peerCertificate().isNull()) {
        fail(FailReason::AuthRefused, authResultName(CastAuthResult::NoPeerCertificate));
        return;
    }
    quint32 words[kNonceWords];
    QRandomGenerator::system()->fillRange(words);
    m_nonce =
        QByteArray(reinterpret_cast<const char *>(words), static_cast<qsizetype>(sizeof(words)));
    CastMessage challenge =
        textMessage(QString(kCastPlatformId), kCastNsDeviceAuth, encodeAuthChallenge(m_nonce));
    challenge.binary = true;
    if (!sendFrame(challenge))
        fail(FailReason::Lost, QStringLiteral("write"));
}

void CastChannel::onReadyRead()
{
    const auto alive = [this] {
        return m_state == State::Authenticating || m_state == State::Open;
    };
    while (alive() && m_socket.bytesAvailable() > 0) {
        m_framer.append(m_socket.read(kReadChunkBytes));
        while (alive()) {
            const std::optional<QByteArray> body = m_framer.next();
            if (!body)
                break;
            handleFrame(*body);
        }
        if (m_framer.failed())
            fail(FailReason::Protocol, QStringLiteral("frame length"));
    }
}

void CastChannel::onSocketClosed()
{
    const QString detail = socketDetail(m_socket.error());
    if (m_state == State::Connecting)
        fail(FailReason::ConnectFailed, detail);
    else if (m_state == State::Authenticating || m_state == State::Open)
        fail(FailReason::Lost, detail);
}

bool CastChannel::countFrame()
{
    const qint64 now = m_clock.elapsed();
    m_frameTimes.push_back(now);
    while (!m_frameTimes.empty() && now - m_frameTimes.front() > m_options.floodWindowMs)
        m_frameTimes.pop_front();
    if (static_cast<int>(m_frameTimes.size()) > m_options.floodFrames) {
        fail(FailReason::Flood, QStringLiteral("too many frames"));
        return false;
    }
    return true;
}

void CastChannel::handleFrame(const QByteArray &body)
{
    if (!countFrame())
        return;
    const std::optional<CastMessage> message = decodeCastMessage(body);
    if (!message) {
        fail(FailReason::Protocol, QStringLiteral("malformed message"));
        return;
    }
    m_lastFrameMs = m_clock.elapsed();
    if (m_state == State::Authenticating) {
        if (message->ns == kCastNsDeviceAuth)
            handleAuth(*message);
        return;
    }
    const std::optional<QJsonObject> json = parseObject(*message);
    if (!json)
        return;
    if (message->ns == kCastNsHeartbeat || message->ns == kCastNsConnection)
        handleHeartbeatAndConnection(*message, *json);
    if (message->ns == kCastNsHeartbeat)
        return;
    if (const quint32 id = requestIdOf(*json); id != 0)
        std::erase_if(m_pending, [id](const Pending &pending) { return pending.id == id; });
    Q_EMIT messageReceived(message->sourceId, message->ns, *json);
}

void CastChannel::handleAuth(const CastMessage &message)
{
    const std::optional<CastAuthMessage> answer = decodeAuthMessage(message.payload);
    if (!answer) {
        fail(FailReason::AuthRefused, QStringLiteral("malformed"));
        return;
    }
    const CastAuthVerdict verdict =
        verifyDeviceAuth(m_options.trust, m_nonce, m_socket.peerCertificate().toDer(), *answer,
                         m_options.requireVideo);
    for (const QString &warning : verdict.warnings)
        qCInfo(channelLog) << "device authentication:" << warning;
    if (verdict.result != CastAuthResult::Ok) {
        fail(FailReason::AuthRefused, authResultName(verdict.result));
        return;
    }
    m_stageTimer.stop();
    if (answer->response)
        m_leafHash = QCryptographicHash::hash(answer->response->clientAuthCertificate,
                                              QCryptographicHash::Sha256);
    m_lastFrameMs = m_clock.elapsed();
    m_lastPingMs = m_lastFrameMs;
    m_ticker.start();
    setState(State::Open);
    if (m_state == State::Open)
        Q_EMIT opened();
}

void CastChannel::handleHeartbeatAndConnection(const CastMessage &message, const QJsonObject &json)
{
    const QString type = json.value(QLatin1String("type")).toString();
    if (message.ns == kCastNsHeartbeat) {
        if (type == QLatin1String("PING"))
            sendFrame(textMessage(message.sourceId, kCastNsHeartbeat, kPongPayload));
        return;
    }
    if (type == QLatin1String("CLOSE")) {
        m_virtualConnections.removeAll(message.sourceId);
        Q_EMIT virtualConnectionClosed(message.sourceId);
    }
}

void CastChannel::tick()
{
    if (m_state != State::Open)
        return;
    const qint64 now = m_clock.elapsed();
    if (now - m_lastFrameMs >= m_options.livenessMs) {
        fail(FailReason::Lost, QStringLiteral("silent"));
        return;
    }
    if (now - m_lastPingMs >= m_options.pingIntervalMs) {
        m_lastPingMs = now;
        QStringList targets{QString(kCastPlatformId)};
        for (const QString &destination : std::as_const(m_virtualConnections)) {
            if (!targets.contains(destination))
                targets.append(destination);
        }
        for (const QString &destination : std::as_const(targets))
            sendFrame(textMessage(destination, kCastNsHeartbeat, kPingPayload));
    }
    QList<quint32> expired;
    for (auto it = m_pending.begin(); it != m_pending.end();) {
        if (it->deadlineMs <= now) {
            expired.append(it->id);
            it = m_pending.erase(it);
        } else {
            ++it;
        }
    }
    for (const quint32 id : std::as_const(expired))
        Q_EMIT requestTimedOut(id);
}

bool CastChannel::sendFrame(const CastMessage &message)
{
    const QByteArray frame = frameCastMessage(message);
    if (frame.isEmpty() || m_socket.state() != QAbstractSocket::ConnectedState)
        return false;
    return m_socket.write(frame) == frame.size();
}

} // namespace Kinefin
