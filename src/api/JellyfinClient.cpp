// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "JellyfinClient.h"

#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrlQuery>

namespace Kinefin {
namespace {
constexpr int MaxResponseBytes = 1'048'576;
constexpr int MaxPublicRedirects = 3;
const QString publicSuffix = QStringLiteral("/System/Info/Public");

bool validServerUrl(const QUrl &url)
{
    return url.isValid() && !url.isRelative()
           && (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https"))
           && !url.host().isEmpty() && url.userName().isEmpty() && url.password().isEmpty()
           && !url.hasQuery() && !url.hasFragment() && url.port(-1) != 0;
}

bool validHeaderField(const QString &field)
{
    if (field.isEmpty() || field.size() > 128)
        return false;
    for (const QChar c : field) {
        if (c.unicode() < 0x20 || c.unicode() == 0x7f || c == QLatin1Char('"')
            || c == QLatin1Char('\\'))
            return false;
    }
    return true;
}

QUrl normalizedBase(QUrl url)
{
    QString path = url.path();
    while (path.endsWith(QLatin1Char('/')))
        path.chop(1);
    url.setPath(path);
    return url;
}

// Public discovery is unauthenticated, so a scheme/port upgrade is safe on the same hostname. Never accept a different host or a TLS downgrade.
bool allowedPublicRedirect(const QUrl &from, const QUrl &to)
{
    if (!validServerUrl(to) || from.host().compare(to.host(), Qt::CaseInsensitive) != 0)
        return false;
    if (from.scheme() == QLatin1String("https") && to.scheme() != QLatin1String("https"))
        return false;
    return to.path().endsWith(publicSuffix);
}

QUrl baseForPublicEndpoint(QUrl url)
{
    if (!validServerUrl(url) || !url.path().endsWith(publicSuffix))
        return {};
    QString path = url.path();
    path.chop(publicSuffix.size());
    url.setPath(path);
    return normalizedBase(url);
}

bool isLoopback(const QUrl &url)
{
    if (url.host().compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0)
        return true;
    QHostAddress address;
    return address.setAddress(url.host()) && address.isLoopback();
}

bool parseObject(QNetworkReply *reply, QJsonObject *object)
{
    const QByteArray bytes = reply->read(MaxResponseBytes + 1);
    if (bytes.size() > MaxResponseBytes)
        return false;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return false;
    *object = document.object();
    return true;
}
} // namespace

bool isValidHeaderField(const QString &field)
{
    return validHeaderField(field);
}

JellyfinClient::JellyfinClient(QObject *parent) : QObject(parent)
{
    m_reportRetryTimer.setSingleShot(true);
    connect(&m_reportRetryTimer, &QTimer::timeout, this, [this] { sendNextPlaybackReport(); });
    connect(this, &JellyfinClient::loginSucceeded, this, [this] { reportCapabilities(); });
}

JellyfinClient::~JellyfinClient()
{
    disconnectFromServer();
}

bool JellyfinClient::setDeviceIdentity(const QString &name, const QString &id,
                                       const QString &version)
{
    if (m_state == State::Authenticating || m_state == State::Authenticated
        || m_state == State::QuickStarting || m_state == State::QuickPending
        || m_state == State::QuickAuthenticating || !validHeaderField(name) || !validHeaderField(id)
        || !validHeaderField(version))
        return false;
    m_deviceName = name;
    m_deviceId = id;
    m_version = version;
    return true;
}

void JellyfinClient::setAllowInsecureHttp(bool allowed)
{
    // This opt-in is scoped to the current server and resets on disconnect.
    if (m_state == State::Disconnected || m_state == State::Probing || m_state == State::Ready)
        m_allowInsecureHttp = allowed;
}

bool JellyfinClient::connectToServer(const QUrl &baseUrl)
{
    if (!validServerUrl(baseUrl))
        return false;
    disconnectFromServer();
    m_base = normalizedBase(baseUrl);
    m_state = State::Probing;
    startProbe(endpoint(publicSuffix), m_generation, 0);
    return true;
}

void JellyfinClient::startProbe(const QUrl &url, quint64 generation, int redirects)
{
    QNetworkReply *reply = m_network.get(requestFor(url, false));
    attach(reply, generation, [this, generation, redirects](QNetworkReply *finished) {
        const int status = finished->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QVariant redirect = finished->attribute(QNetworkRequest::RedirectionTargetAttribute);
        if (status >= 300 && status < 400 && redirect.isValid()) {
            const QUrl target = finished->url().resolved(redirect.toUrl());
            if (redirects >= MaxPublicRedirects
                || !allowedPublicRedirect(finished->url(), target)) {
                failProbe(status, QStringLiteral("Unsafe or excessive server redirect"));
                return;
            }
            startProbe(target, generation, redirects + 1);
            return;
        }
        if (status != 200 || finished->error() != QNetworkReply::NoError) {
            failProbe(status, QStringLiteral("Server identification request failed"));
            return;
        }
        QJsonObject data;
        if (!parseObject(finished, &data)
            || data.value(QStringLiteral("Id")).toString().trimmed().isEmpty()) {
            failProbe(status, QStringLiteral("Invalid Jellyfin server identity"));
            return;
        }
        const QUrl resolvedBase = baseForPublicEndpoint(finished->url());
        if (resolvedBase.isEmpty()) {
            failProbe(status, QStringLiteral("Invalid resolved API base"));
            return;
        }
        m_base = resolvedBase;
        m_serverId = data.value(QStringLiteral("Id")).toString();
        m_state = State::Ready;
        Q_EMIT serverReady(m_serverId);
    });
}

bool JellyfinClient::transportAllowsAuthentication() const
{
    return m_base.scheme() == QLatin1String("https") || isLoopback(m_base) || m_allowInsecureHttp;
}

QByteArray JellyfinClient::identityAuthorization() const
{
    return QByteArray("MediaBrowser Client=\"Kinefin\", Device=\"") + m_deviceName.toUtf8()
           + "\", DeviceId=\"" + m_deviceId.toUtf8() + "\", Version=\"" + m_version.toUtf8() + "\"";
}

QByteArray JellyfinClient::tokenAuthorization() const
{
    return identityAuthorization() + ", Token=\"" + m_token + "\"";
}

bool JellyfinClient::authenticate(const QString &username, const QString &password)
{
    if (m_state != State::Ready || m_deviceId.isEmpty() || username.isEmpty() || password.isEmpty())
        return false;
    if (!transportAllowsAuthentication()) {
        Q_EMIT requestFailed(
            0, QStringLiteral("Credentials require HTTPS or an explicit insecure-HTTP opt-in"));
        return false;
    }
    m_state = State::Authenticating;
    const QJsonObject data{{QStringLiteral("Username"), username},
                           {QStringLiteral("Pw"), password}};
    QNetworkRequest request =
        requestFor(endpoint(QStringLiteral("/Users/AuthenticateByName")), false);
    request.setRawHeader("Authorization", identityAuthorization());
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply *reply =
        m_network.post(request, QJsonDocument(data).toJson(QJsonDocument::Compact));
    attach(reply, m_generation,
           [this](QNetworkReply *finished) { finishAuthentication(finished); });
    return true;
}

bool JellyfinClient::initiateQuickConnect()
{
    if (m_state != State::Ready || m_deviceId.isEmpty())
        return false;
    // Both the eventual bearer token and the Quick Connect secret are credentials.
    if (!transportAllowsAuthentication()) {
        Q_EMIT requestFailed(
            0, QStringLiteral("Quick Connect requires HTTPS or an explicit insecure-HTTP opt-in"));
        return false;
    }
    m_state = State::QuickStarting;
    QNetworkRequest request = requestFor(endpoint(QStringLiteral("/QuickConnect/Initiate")), false);
    request.setRawHeader("Authorization", identityAuthorization());
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply *reply = m_network.post(request, QByteArrayLiteral("{}"));
    attach(reply, m_generation, [this](QNetworkReply *finished) {
        const int status = finished->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status != 200 || finished->error() != QNetworkReply::NoError) {
            m_state = State::Ready;
            Q_EMIT requestFailed(status, QStringLiteral("Quick Connect initiation failed"));
            return;
        }
        QJsonObject data;
        if (!parseObject(finished, &data)) {
            m_state = State::Ready;
            Q_EMIT requestFailed(status,
                                 QStringLiteral("Invalid Quick Connect initiation response"));
            return;
        }
        const QString code = data.value(QStringLiteral("Code")).toString();
        const QString secret = data.value(QStringLiteral("Secret")).toString();
        const QJsonValue approved = data.value(QStringLiteral("Authenticated"));
        if (!validHeaderField(code) || code.size() > 32 || !validHeaderField(secret)
            || (!approved.isUndefined() && !approved.isBool()) || approved.toBool()) {
            m_state = State::Ready;
            Q_EMIT requestFailed(status, QStringLiteral("Invalid Quick Connect pairing identity"));
            return;
        }
        m_quickConnectSecret = secret.toUtf8();
        m_state = State::QuickPending;
        // Expose only the short user-facing pairing code, never the secret.
        Q_EMIT quickConnectCodeReady(code);
    });
    return true;
}

bool JellyfinClient::pollQuickConnect()
{
    if (m_state != State::QuickPending || m_quickPollInFlight || m_quickConnectSecret.isEmpty())
        return false;
    QUrl url = endpoint(QStringLiteral("/QuickConnect/Connect"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("secret"), QString::fromUtf8(m_quickConnectSecret));
    url.setQuery(query);
    m_quickPollInFlight = true;
    QNetworkReply *reply = m_network.get(requestFor(url, false));
    attach(reply, m_generation, [this](QNetworkReply *finished) {
        m_quickPollInFlight = false;
        const int status = finished->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status != 200 || finished->error() != QNetworkReply::NoError) {
            cancelQuickConnect();
            Q_EMIT requestFailed(status, QStringLiteral("Quick Connect status request failed"));
            return;
        }
        QJsonObject data;
        if (!parseObject(finished, &data)
            || !data.value(QStringLiteral("Authenticated")).isBool()) {
            cancelQuickConnect();
            Q_EMIT requestFailed(status, QStringLiteral("Invalid Quick Connect status response"));
            return;
        }
        const QJsonValue echoedSecret = data.value(QStringLiteral("Secret"));
        if (!echoedSecret.isUndefined()
            && (!echoedSecret.isString()
                || echoedSecret.toString().toUtf8() != m_quickConnectSecret)) {
            cancelQuickConnect();
            Q_EMIT requestFailed(status,
                                 QStringLiteral("Mismatched Quick Connect pairing identity"));
            return;
        }
        if (!data.value(QStringLiteral("Authenticated")).toBool()) {
            Q_EMIT quickConnectAwaitingApproval();
            return;
        }
        exchangeQuickConnectSecret();
    });
    return true;
}

void JellyfinClient::exchangeQuickConnectSecret()
{
    if (m_state != State::QuickPending || m_quickConnectSecret.isEmpty())
        return;
    m_state = State::QuickAuthenticating;
    QNetworkRequest request =
        requestFor(endpoint(QStringLiteral("/Users/AuthenticateWithQuickConnect")), false);
    request.setRawHeader("Authorization", identityAuthorization());
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    const QJsonObject data{{QStringLiteral("Secret"), QString::fromUtf8(m_quickConnectSecret)}};
    QNetworkReply *reply =
        m_network.post(request, QJsonDocument(data).toJson(QJsonDocument::Compact));
    attach(reply, m_generation,
           [this](QNetworkReply *finished) { finishAuthentication(finished); });
}

void JellyfinClient::finishAuthentication(QNetworkReply *finished)
{
    const bool quick = m_state == State::QuickAuthenticating;
    const int status = finished->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    clearQuickConnectSecret();
    m_state = State::Ready;
    if (status != 200 || finished->error() != QNetworkReply::NoError) {
        Q_EMIT requestFailed(status, quick ? QStringLiteral("Quick Connect authentication failed")
                                           : QStringLiteral("Authentication failed"));
        return;
    }
    QJsonObject result;
    if (!parseObject(finished, &result)) {
        Q_EMIT requestFailed(status, QStringLiteral("Invalid authentication response"));
        return;
    }
    const QString token = result.value(QStringLiteral("AccessToken")).toString();
    const QString userId =
        result.value(QStringLiteral("User")).toObject().value(QStringLiteral("Id")).toString();
    const QString reportedServerId = result.value(QStringLiteral("ServerId")).toString();
    if (token.isEmpty() || userId.isEmpty() || reportedServerId != m_serverId
        || !validHeaderField(token)) {
        Q_EMIT requestFailed(status, QStringLiteral("Invalid authentication identity"));
        return;
    }
    m_token = token.toUtf8();
    m_userId = userId;
    m_state = State::Authenticated;
    Q_EMIT loginSucceeded(m_userId);
}

void JellyfinClient::cancelQuickConnect()
{
    if (m_state != State::QuickStarting && m_state != State::QuickPending
        && m_state != State::QuickAuthenticating)
        return;
    abortPending();
    clearQuickConnectSecret();
    m_state = State::Ready;
}

bool JellyfinClient::isQuickConnectPending() const
{
    return m_state == State::QuickStarting || m_state == State::QuickPending
           || m_state == State::QuickAuthenticating;
}

void JellyfinClient::clearQuickConnectSecret()
{
    m_quickConnectSecret.fill('\0');
    m_quickConnectSecret.clear();
    m_quickPollInFlight = false;
}

bool JellyfinClient::fetchUserViews()
{
    if (m_state != State::Authenticated)
        return false;
    QUrl url = endpoint(QStringLiteral("/UserViews"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("userId"), m_userId);
    url.setQuery(query);
    QNetworkReply *reply = m_network.get(requestFor(url, true));
    attach(reply, m_generation, [this](QNetworkReply *finished) {
        const int status = finished->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 401) {
            invalidateSession();
            Q_EMIT requestFailed(status, QStringLiteral("Authentication expired"));
            return;
        }
        if (status != 200 || finished->error() != QNetworkReply::NoError) {
            Q_EMIT requestFailed(status, QStringLiteral("Library request failed"));
            return;
        }
        QJsonObject data;
        if (!parseObject(finished, &data) || !data.value(QStringLiteral("Items")).isArray()
            || !data.value(QStringLiteral("TotalRecordCount")).isDouble()) {
            Q_EMIT requestFailed(status, QStringLiteral("Invalid library response"));
            return;
        }
        const int total = data.value(QStringLiteral("TotalRecordCount")).toInt(-1);
        const QJsonArray items = data.value(QStringLiteral("Items")).toArray();
        if (total < items.size()) {
            Q_EMIT requestFailed(status, QStringLiteral("Invalid library pagination"));
            return;
        }
        Q_EMIT userViewsReady(items, total);
    });
    return true;
}

bool JellyfinClient::requestPlaybackInfo(const QString &itemId, qint64 startTimeTicks,
                                         const QJsonObject &deviceProfile,
                                         qint64 maxStreamingBitrate)
{
    if (m_state != State::Authenticated || !isValidJellyfinId(itemId) || startTimeTicks < 0
        || maxStreamingBitrate <= 0 || deviceProfile.isEmpty())
        return false;
    cancelPlaybackInfo();
    const quint64 serial = m_playbackSerial;
    const QJsonObject body{{QStringLiteral("UserId"), m_userId},
                           {QStringLiteral("StartTimeTicks"), startTimeTicks},
                           {QStringLiteral("MaxStreamingBitrate"), maxStreamingBitrate},
                           {QStringLiteral("DeviceProfile"), deviceProfile}};
    QNetworkRequest request = requestFor(
        endpoint(QStringLiteral("/Items/") + itemId + QStringLiteral("/PlaybackInfo")), true);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply *reply =
        m_network.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    m_playbackReply = reply;
    attach(reply, m_generation, [this, serial, itemId](QNetworkReply *finished) {
        if (serial != m_playbackSerial)
            return;
        const int status = finished->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 401) {
            invalidateSession();
            Q_EMIT playbackInfoFailed(itemId, status, QStringLiteral("Authentication expired"));
            return;
        }
        if (status != 200 || finished->error() != QNetworkReply::NoError) {
            Q_EMIT playbackInfoFailed(itemId, status, QStringLiteral("Playback request failed"));
            return;
        }
        QJsonObject data;
        if (!parseObject(finished, &data)) {
            Q_EMIT playbackInfoFailed(itemId, status, QStringLiteral("Invalid playback response"));
            return;
        }
        const SourceSelection selection = selectPlaybackSource(data, itemId, m_base, m_token);
        if (!selection.source) {
            Q_EMIT playbackInfoFailed(itemId, status, selection.failure);
            return;
        }
        Q_EMIT playbackInfoReady(*selection.source);
    });
    return true;
}

namespace {
QString reportRoute(PlaybackReport::Kind kind)
{
    switch (kind) {
    case PlaybackReport::Kind::Start:
        return QStringLiteral("/Sessions/Playing");
    case PlaybackReport::Kind::Progress:
        return QStringLiteral("/Sessions/Playing/Progress");
    case PlaybackReport::Kind::Stopped:
        break;
    }
    return QStringLiteral("/Sessions/Playing/Stopped");
}

bool retryableStatus(int status)
{
    return status == 0 || status == 502 || status == 503 || status == 504;
}
} // namespace

bool JellyfinClient::reportPlayback(const PlaybackReport &report)
{
    const std::optional<QJsonObject> body = playbackReportBody(report);
    if (m_state != State::Authenticated || !body)
        return false;
    // The first queued report is in flight, or waiting for its retry: only the ones behind it may be coalesced.
    const qsizetype waiting = (m_reportBusy || m_reportRetryTimer.isActive()) ? 1 : 0;
    if (report.kind == PlaybackReport::Kind::Stopped) {
        for (qsizetype i = m_reportQueue.size() - 1; i >= waiting; --i) {
            if (m_reportQueue.at(i).report.kind == PlaybackReport::Kind::Progress)
                m_reportQueue.removeAt(i);
        }
    } else if (report.kind == PlaybackReport::Kind::Progress && m_reportQueue.size() > waiting
               && m_reportQueue.constLast().report.kind == PlaybackReport::Kind::Progress) {
        m_reportQueue.removeLast();
    }
    m_reportQueue.append({report, QJsonDocument(*body).toJson(QJsonDocument::Compact), 0});
    sendNextPlaybackReport();
    return true;
}

bool JellyfinClient::hasPendingPlaybackReports() const
{
    return !m_reportQueue.isEmpty();
}

void JellyfinClient::setPlaybackReportRetryDelay(std::chrono::milliseconds delay)
{
    m_reportRetryDelay = delay;
}

void JellyfinClient::sendNextPlaybackReport()
{
    // One request at a time, and nothing while the first report waits for its retry.
    if (m_state != State::Authenticated || m_reportQueue.isEmpty() || m_reportBusy
        || m_reportRetryTimer.isActive())
        return;
    m_reportBusy = true;
    const QueuedReport &next = m_reportQueue.constFirst();
    QNetworkRequest request = requestFor(endpoint(reportRoute(next.report.kind)), true);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply *reply = m_network.post(request, next.body);
    attach(reply, m_generation,
           [this](QNetworkReply *finished) { finishPlaybackReport(finished); });
}

void JellyfinClient::finishPlaybackReport(QNetworkReply *reply)
{
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 401) {
        invalidateSession(); // drops the queue
        return;
    }
    QueuedReport &current = m_reportQueue.first();
    const bool delivered =
        status >= 200 && status < 300 && reply->error() == QNetworkReply::NoError;
    if (!delivered && current.report.kind == PlaybackReport::Kind::Stopped && current.attempts == 0
        && retryableStatus(status)) {
        // The one retry of a Stopped: ordering holds, nothing else is sent meanwhile.
        current.attempts = 1;
        m_reportBusy = false;
        m_reportRetryTimer.start(m_reportRetryDelay);
        return;
    }
    m_reportQueue.removeFirst();
    m_reportBusy = false;
    if (m_reportQueue.isEmpty())
        Q_EMIT playbackReportsSettled();
    else
        sendNextPlaybackReport();
}

bool JellyfinClient::reportCapabilities()
{
    if (m_state != State::Authenticated)
        return false;
    QNetworkRequest request =
        requestFor(endpoint(QStringLiteral("/Sessions/Capabilities/Full")), true);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply *reply =
        m_network.post(request, QJsonDocument(clientCapabilitiesBody(m_mediaControlAnnounced))
                                    .toJson(QJsonDocument::Compact));
    attach(reply, m_generation, [this](QNetworkReply *finished) {
        if (finished->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 401)
            invalidateSession();
    });
    return true;
}

void JellyfinClient::cancelPlaybackInfo()
{
    // Bumping the serial makes the handler of the aborted reply a no-op: no signal for a dropped request.
    ++m_playbackSerial;
    if (m_playbackReply)
        m_playbackReply->abort();
    m_playbackReply.clear();
}

QByteArray JellyfinClient::streamAuthorization() const
{
    if (m_state != State::Authenticated)
        return {};
    return tokenAuthorization();
}

QNetworkRequest JellyfinClient::webSocketRequest() const
{
    if (m_state != State::Authenticated || m_base.isEmpty())
        return {};
    QUrl url = m_base;
    url.setPath(m_base.path() + QStringLiteral("/socket"));
    // The authenticated transport convention: https upgrades, and plain ws only where credentials may travel in the clear (loopback or the explicit opt-in). Authentication itself is impossible otherwise.
    url.setScheme(url.scheme() == QLatin1String("https") ? QStringLiteral("wss")
                                                         : QStringLiteral("ws"));
    if (url.scheme() == QLatin1String("ws") && !transportAllowsAuthentication())
        return {};
    QNetworkRequest request(url);
    request.setRawHeader("Authorization", tokenAuthorization());
    return request;
}

bool JellyfinClient::verifySession()
{
    if (m_state != State::Authenticated || m_verifyInFlight)
        return false;
    m_verifyInFlight = true;
    QNetworkReply *reply = m_network.get(requestFor(endpoint(QStringLiteral("/Users/Me")), true));
    attach(reply, m_generation, [this](QNetworkReply *finished) {
        m_verifyInFlight = false;
        if (finished->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 401)
            invalidateSession();
    });
    return true;
}

void JellyfinClient::attach(QNetworkReply *reply, quint64 generation,
                            const std::function<void(QNetworkReply *)> &handler)
{
    m_pending.insert(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation, handler] {
        m_pending.remove(reply);
        reply->deleteLater();
        if (generation == m_generation)
            handler(reply);
    });
}

void JellyfinClient::abortPending()
{
    ++m_generation;
    // Reports die with the pending requests, quietly: the generation bump keeps their handlers from running. The same goes for a session re-check whose flag would otherwise stay set forever.
    m_reportQueue.clear();
    m_reportBusy = false;
    m_reportRetryTimer.stop();
    m_verifyInFlight = false;
    const auto pending = m_pending;
    m_pending.clear();
    for (QNetworkReply *reply : pending)
        reply->abort();
}

void JellyfinClient::clearToken()
{
    const bool hadToken = !m_token.isEmpty();
    m_token.fill('\0');
    m_token.clear();
    m_userId.clear();
    if (hadToken)
        Q_EMIT credentialsCleared();
}

void JellyfinClient::setMediaControlAnnounced(bool announced)
{
    m_mediaControlAnnounced = announced;
}

void JellyfinClient::invalidateSession()
{
    if (m_state != State::Authenticated)
        return;
    abortPending();
    clearToken();
    m_state = State::Ready;
    Q_EMIT sessionExpired();
}

void JellyfinClient::failProbe(int status, const QString &reason)
{
    abortPending();
    clearToken();
    clearQuickConnectSecret();
    m_state = State::Disconnected;
    m_serverId.clear();
    m_base = {};
    Q_EMIT requestFailed(status, reason);
}

void JellyfinClient::disconnectFromServer()
{
    abortPending();
    clearToken();
    clearQuickConnectSecret();
    m_serverId.clear();
    m_base = {};
    m_state = State::Disconnected;
    m_allowInsecureHttp = false;
}

QUrl JellyfinClient::endpoint(const QString &path) const
{
    QUrl url = m_base;
    url.setPath(m_base.path() + path);
    return url;
}

QNetworkRequest JellyfinClient::requestFor(const QUrl &url, bool authenticated) const
{
    QNetworkRequest request(url);
    // Tokens are never propagated by Qt's automatic redirect handling.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(15000);
    request.setRawHeader("Accept", "application/json");
    if (authenticated) {
        request.setRawHeader("Authorization", tokenAuthorization());
    }
    return request;
}

bool JellyfinClient::isAuthenticated() const
{
    return m_state == State::Authenticated;
}
QUrl JellyfinClient::serverUrl() const
{
    return m_base;
}
QString JellyfinClient::serverId() const
{
    return m_serverId;
}
QString JellyfinClient::userId() const
{
    return m_userId;
}
QString JellyfinClient::deviceId() const
{
    return m_deviceId;
}
QString JellyfinClient::deviceName() const
{
    return m_deviceName;
}
QString JellyfinClient::accessToken() const
{
    return m_state == State::Authenticated ? QString::fromUtf8(m_token) : QString();
}

bool JellyfinClient::canSendCredentials() const
{
    return transportAllowsAuthentication();
}

bool JellyfinClient::restoreSession(const QString &userId, const QString &token)
{
    if (m_state != State::Ready || m_deviceId.isEmpty() || userId.isEmpty() || token.isEmpty()
        || !validHeaderField(token) || !transportAllowsAuthentication())
        return false;
    m_token = token.toUtf8();
    m_userId = userId;
    m_state = State::Authenticated;
    Q_EMIT loginSucceeded(m_userId);
    return true;
}

} // namespace Kinefin
