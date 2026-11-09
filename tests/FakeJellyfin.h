// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// An in-process fake Jellyfin server and the helpers shared by the API client tests. Header-only on purpose: every test target includes it (KINEFIN_SOURCE_DIR must be defined for the fixtures).
#pragma once

#include "api/JellyfinClient.h"

#include "FakeSocketConnection.h"

#include <QFile>
#include <QHash>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrlQuery>
#include <QtTest>

namespace {

inline QByteArray fixture(const QString &name)
{
    QFile file(QString::fromUtf8(KINEFIN_SOURCE_DIR) + QStringLiteral("/tests/fixtures/") + name
               + QStringLiteral(".json"));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

class FakeJellyfin {
  public:
    FakeJellyfin()
    {
        QObject::connect(&m_server, &QTcpServer::newConnection, &m_server, [this] {
            while (QTcpSocket *socket = m_server.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::readyRead, socket,
                                 [this, socket] { readRequest(socket); });
                QObject::connect(socket, &QTcpSocket::disconnected, socket, [this, socket] {
                    m_buffers.remove(socket);
                    socket->deleteLater();
                });
            }
        });
    }

    bool listen(const QHostAddress &address = QHostAddress::LocalHost)
    {
        return m_server.listen(address);
    }

    ~FakeJellyfin() { qDeleteAll(m_wsConnections); }

    QUrl url(const QString &base = QString()) const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(m_server.serverPort()).arg(base));
    }

    // The login response body, with the access token substituted on later successful logins when rotation is on.
    QByteArray authenticateBody() const
    {
        auto document = QJsonDocument::fromJson(fixture(QStringLiteral("authenticate_success")));
        if (rotateSecondLoginToken && successfulLogins >= 2) {
            QJsonObject object = document.object();
            object.insert(QStringLiteral("AccessToken"), QString::fromUtf8(secondLoginToken));
            document.setObject(object);
        }
        return document.toJson(QJsonDocument::Compact);
    }

    bool invalidIdentity = false;
    bool denyLogin = false;
    bool denyViews = false;
    bool redirectPublicOffHost = false;
    bool relocatePublicOnSameHost = false;
    bool redirectViewsOffHost = false;
    bool holdViews = false;
    bool disableQuickConnect = false;
    bool invalidQuickConnectCode = false;
    bool rejectQuickConnectStatus = false;
    bool mismatchQuickConnectSecret = false;
    bool approveQuickConnect = false;
    bool denyQuickConnectExchange = false;
    bool holdQuickConnectPoll = false;
    int quickInitiationRequests = 0;
    int quickStatusRequests = 0;
    int quickExchangeRequests = 0;
    QByteArray lastQuickExchangeBody;
    QByteArray lastQuickExchangeAuth;
    QByteArray lastQuickPollAuth;
    QString lastQuickPollSecret;
    int viewRequests = 0;
    QByteArray lastViewsAuth;
    QByteArray lastLoginAuth;
    // PlaybackInfo: the fixture to answer with, or a status / raw body override.
    QString playbackInfoFixture = QStringLiteral("playbackinfo_direct");
    int playbackInfoStatus = 0;
    QByteArray playbackInfoRawBody;
    bool holdPlaybackInfo = false;
    bool redirectPlaybackInfo = false;
    int playbackInfoRequests = 0;
    QByteArray lastPlaybackInfoRoute;
    QByteArray lastPlaybackInfoAuth;
    QByteArray lastPlaybackInfoBody;
    QByteArray lastPlaybackInfoContentType;
    // Playback reports: an ordered log of what arrived, a status for all of them, an optional different status for the first Stopped, and a hold that keeps the answers back until releaseHeld().
    struct ReportLog {
        QByteArray route;
        QByteArray body;
        QByteArray auth;
        QByteArray contentType;
    };
    QList<ReportLog> reportLog;
    int reportStatus = 204;
    int firstStoppedStatus = 0;
    int stoppedRequests = 0;
    bool holdReports = false;
    bool redirectReports = false;
    int capabilitiesRequests = 0;
    QByteArray lastCapabilitiesBody;
    QByteArray lastCapabilitiesAuth;
    // WebSocket upgrade: the attempt is recorded at the header boundary before any accept decision, so tests assert the handshake itself (`Token` only, never `api_key` or a query).
    int upgradeAttempts = 0;
    QString lastSocketRoute;
    QByteArray lastSocketAuth;
    // Everything after the '?' of the upgrade request line; must stay empty.
    QString lastSocketQuery;
    // 0 accepts the 101; otherwise the upgrade is answered with this status as plain HTTP.
    int socketUpgradeStatus = 0;
    // Keep the TCP connection open but answer nothing: the client-side connect timeout shape.
    bool holdSocketUpgrade = false;
    // Login tokens: the fixture one on the first success, a different one on later successes.
    bool rotateSecondLoginToken = false;
    QByteArray secondLoginToken = "f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7";
    int successfulLogins = 0;
    // GET /Users/Me (the session re-verification): counts and identity, with a deny switch.
    int usersMeRequests = 0;
    QByteArray lastUsersMeAuth;
    bool denyUsersMe = false;

    // The most recent accepted WebSocket connection (nullptr until one handshake completes).
    FakeSocketConnection *lastConnection() const
    {
        return m_wsConnections.isEmpty() ? nullptr : m_wsConnections.constLast();
    }

    void releaseHeld()
    {
        const auto held = m_held;
        m_held.clear();
        for (const QPointer<QTcpSocket> &socket : held) {
            if (socket)
                respond(socket, reportStatus, {});
        }
    }

  private:
    static void respond(QTcpSocket *socket, int status, const QByteArray &body,
                        const QByteArray &extraHeaders = {})
    {
        QByteArray result = "HTTP/1.1 " + QByteArray::number(status)
                            + (status == 200 ? " OK\r\n" : " Response\r\n");
        result += "Content-Type: application/json\r\nContent-Length: "
                  + QByteArray::number(body.size()) + "\r\n";
        result += extraHeaders + "Connection: close\r\n\r\n" + body;
        socket->write(result);
        socket->disconnectFromHost();
    }

    void readRequest(QTcpSocket *socket)
    {
        // Once a WebSocket handshake is accepted, the bytes belong to the frame parser.
        if (FakeSocketConnection *connection = m_wsTaken.value(socket)) {
            connection->drain();
            return;
        }
        if (m_wsTaken.contains(socket))
            return;
        QByteArray &input = m_buffers[socket];
        input.append(socket->readAll());
        const qsizetype end = input.indexOf("\r\n\r\n");
        if (end < 0)
            return;
        const QByteArray headers = input.left(end + 2);
        const QList<QByteArray> lines = headers.split('\n');
        int contentLength = 0;
        QByteArray authorization;
        QByteArray contentType;
        QByteArray websocketKey;
        bool upgradeRequest = false;
        for (const QByteArray &raw : lines) {
            const QByteArray line = raw.trimmed();
            if (line.toLower().startsWith("content-length:"))
                contentLength = line.mid(15).trimmed().toInt();
            if (line.toLower().startsWith("authorization:"))
                authorization = line.mid(14).trimmed();
            if (line.toLower().startsWith("content-type:"))
                contentType = line.mid(13).trimmed();
            if (line.toLower().startsWith("sec-websocket-key:"))
                websocketKey = line.mid(18).trimmed();
            if (line.toLower().startsWith("upgrade:")
                && line.mid(8).trimmed().toLower() == "websocket")
                upgradeRequest = true;
        }
        if (contentLength < 0 || contentLength > 65536) {
            respond(socket, 413, {});
            return;
        }
        const qsizetype bodyStart = end + 4;
        if (input.size() - bodyStart < contentLength)
            return;
        const QByteArray route = lines.constFirst().trimmed().split(' ').value(1);
        const QByteArray method = lines.constFirst().trimmed().split(' ').value(0);
        const QByteArray body = input.mid(bodyStart, contentLength);
        m_buffers.remove(socket);
        if (upgradeRequest) {
            // Guard first so bytes arriving while the handshake is written can never leak back into the HTTP parser.
            m_wsTaken.insert(socket, nullptr);
            ++upgradeAttempts;
            lastSocketRoute = QString::fromUtf8(route);
            lastSocketAuth = authorization;
            const qsizetype query = route.indexOf('?');
            lastSocketQuery =
                query < 0 ? QString() : QString::fromUtf8(route.mid(qsizetype(query) + 1));
            if (holdSocketUpgrade)
                return;
            if (socketUpgradeStatus != 0) {
                respond(socket, socketUpgradeStatus, {},
                        "Location: http://127.0.0.1:9/socket-relocated\r\n");
                return;
            }
            auto *connection = new FakeSocketConnection(socket, websocketKey);
            m_wsTaken.insert(socket, connection);
            m_wsConnections.append(connection);
            return;
        }
        if (method == "GET" && route.endsWith("/System/Info/Public")) {
            if (redirectPublicOffHost) {
                respond(socket, 302, {},
                        "Location: https://unexpected.invalid/System/Info/Public\r\n");
            } else if (relocatePublicOnSameHost && route.startsWith("/jellyfin/")) {
                respond(socket, 302, {}, "Location: /jellyfin-proxy/System/Info/Public\r\n");
            } else if (invalidIdentity) {
                respond(socket, 200, "{\"Id\":null}");
            } else {
                respond(socket, 200, fixture(QStringLiteral("system_info_public")));
            }
            return;
        }
        if (method == "POST" && route.endsWith("/Users/AuthenticateByName")) {
            lastLoginAuth = authorization;
            if (!QJsonDocument::fromJson(body).isObject()) {
                respond(socket, 400, {});
            } else if (denyLogin) {
                respond(socket, 401, {});
            } else {
                ++successfulLogins;
                respond(socket, 200, authenticateBody());
            }
            return;
        }
        if (method == "POST" && route.endsWith("/QuickConnect/Initiate")) {
            ++quickInitiationRequests;
            if (disableQuickConnect) {
                respond(socket, 401, {});
            } else if (invalidQuickConnectCode) {
                respond(socket, 200, "{\"Code\":\"AB12CD\"}");
            } else {
                respond(socket, 200, fixture(QStringLiteral("quick_connect_initiate")));
            }
            return;
        }
        if (method == "GET" && route.contains("/QuickConnect/Connect?")) {
            ++quickStatusRequests;
            lastQuickPollAuth = authorization;
            const QUrl fullUrl =
                QUrl::fromEncoded(QByteArray(QByteArray("http://127.0.0.1") + route));
            lastQuickPollSecret = QUrlQuery(fullUrl).queryItemValue(QStringLiteral("secret"));
            if (holdQuickConnectPoll)
                return;
            if (rejectQuickConnectStatus
                || lastQuickPollSecret != QStringLiteral("fixture-quick-secret")) {
                respond(socket, 404, {});
            } else if (mismatchQuickConnectSecret) {
                respond(socket, 200, "{\"Authenticated\":true,\"Secret\":\"wrong-secret\"}");
            } else if (approveQuickConnect) {
                respond(socket, 200, fixture(QStringLiteral("quick_connect_approved")));
            } else {
                respond(socket, 200, fixture(QStringLiteral("quick_connect_pending")));
            }
            return;
        }
        if (method == "POST" && route.endsWith("/Users/AuthenticateWithQuickConnect")) {
            ++quickExchangeRequests;
            lastQuickExchangeBody = body;
            lastQuickExchangeAuth = authorization;
            if (denyQuickConnectExchange
                || QJsonDocument::fromJson(body).object().value(QStringLiteral("Secret")).toString()
                       != QStringLiteral("fixture-quick-secret")) {
                respond(socket, 401, {});
            } else {
                ++successfulLogins;
                respond(socket, 200, authenticateBody());
            }
            return;
        }
        if (method == "GET" && route.endsWith("/Users/Me")) {
            ++usersMeRequests;
            lastUsersMeAuth = authorization;
            if (denyUsersMe) {
                respond(socket, 401, {});
            } else {
                respond(socket, 200,
                        "{\"Id\":\"d5ed5d4e2e5447b58e9f2b6f7bd5c96a\",\"Name\":\"bepi\","
                        "\"Policy\":{\"IsAdministrator\":false},\"Configuration\":{}}");
            }
            return;
        }
        if (method == "POST" && route.endsWith("/Sessions/Capabilities/Full")) {
            ++capabilitiesRequests;
            lastCapabilitiesBody = body;
            lastCapabilitiesAuth = authorization;
            respond(socket, 204, {});
            return;
        }
        if (method == "POST"
            && (route.endsWith("/Sessions/Playing") || route.endsWith("/Sessions/Playing/Progress")
                || route.endsWith("/Sessions/Playing/Stopped"))) {
            reportLog.append({route, body, authorization, contentType});
            const bool stopped = route.endsWith("/Stopped");
            if (stopped)
                ++stoppedRequests;
            if (holdReports) {
                m_held.append(socket);
                return;
            }
            if (redirectReports) {
                respond(socket, 302, {},
                        "Location: https://unexpected.invalid/Sessions/Playing\r\n");
            } else if (stopped && stoppedRequests == 1 && firstStoppedStatus != 0) {
                respond(socket, firstStoppedStatus, {});
            } else {
                respond(socket, reportStatus, {});
            }
            return;
        }
        if (method == "POST" && route.endsWith("/PlaybackInfo")) {
            ++playbackInfoRequests;
            lastPlaybackInfoRoute = route;
            lastPlaybackInfoAuth = authorization;
            lastPlaybackInfoBody = body;
            lastPlaybackInfoContentType = contentType;
            if (holdPlaybackInfo)
                return;
            if (redirectPlaybackInfo) {
                respond(socket, 302, {},
                        "Location: https://unexpected.invalid/Items/x/PlaybackInfo\r\n");
            } else if (playbackInfoStatus != 0) {
                respond(socket, playbackInfoStatus, {});
            } else if (!playbackInfoRawBody.isEmpty()) {
                respond(socket, 200, playbackInfoRawBody);
            } else {
                respond(socket, 200, fixture(playbackInfoFixture));
            }
            return;
        }
        if (method == "GET" && route.contains("/UserViews?")) {
            ++viewRequests;
            lastViewsAuth = authorization;
            if (holdViews)
                return;
            if (redirectViewsOffHost) {
                respond(socket, 302, {}, "Location: https://unexpected.invalid/UserViews\r\n");
            } else if (denyViews) {
                respond(socket, 401, {});
            } else {
                respond(socket, 200, fixture(QStringLiteral("user_views")));
            }
            return;
        }
        respond(socket, 404, {});
    }

    // The server comes last so it is destroyed first: its child sockets may still emit `disconnected` while it dies, and the slots touch the members above.
    QHash<QTcpSocket *, QByteArray> m_buffers;
    QList<QPointer<QTcpSocket>> m_held;
    QTcpServer m_server;
    QList<FakeSocketConnection *> m_wsConnections;
    QHash<QTcpSocket *, FakeSocketConnection *> m_wsTaken;
};

inline bool prepare(Kinefin::JellyfinClient &client, FakeJellyfin &server)
{
    if (!client.setDeviceIdentity(QStringLiteral("Kinefin test"), QStringLiteral("stable-test-id"),
                                  QStringLiteral("0.0.1")))
        return false;
    return client.connectToServer(server.url(QStringLiteral("/jellyfin")));
}

const QString kPlayItem = QStringLiteral("c7dd3b1ac7dd3b1a9d42f2ab74f8a1b9");
// The access token of tests/fixtures/authenticate_success.json.
const QByteArray kFixtureToken = "f2f4b7c1ee8a4d0bb2a5f9d1f9e0e2ab";
const QByteArray kTokenField = QByteArray("Token=\"") + kFixtureToken + QByteArray("\"");

// Connects and logs in; QTRY_* macros cannot live in a helper that returns a value.
inline bool login(Kinefin::JellyfinClient &client, FakeJellyfin &server)
{
    QSignalSpy ready(&client, &Kinefin::JellyfinClient::serverReady);
    QSignalSpy loggedIn(&client, &Kinefin::JellyfinClient::loginSucceeded);
    const bool prepared = prepare(client, server);
    if (!prepared)
        qWarning() << "login: prepare failed";
    if (!QTest::qWaitFor([&] { return ready.size() == 1; }, 5000)) {
        qWarning() << "login: serverReady never arrived; prepared =" << prepared
                   << "requests seen by the fake:" << server.upgradeAttempts << server.viewRequests
                   << server.capabilitiesRequests;
        return false;
    }
    const bool ok =
        client.authenticate(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"))
        && QTest::qWaitFor([&] { return loggedIn.size() == 1; }, 5000);
    if (!ok)
        qWarning() << "login: authenticate step failed";
    return ok;
}

// A valid direct-play response plus a padding field: the only thing wrong with it is its size.
inline QByteArray paddedPlaybackInfo(int paddingBytes)
{
    QJsonObject object =
        QJsonDocument::fromJson(fixture(QStringLiteral("playbackinfo_direct"))).object();
    object.insert(QStringLiteral("Padding"), QString(paddingBytes, QLatin1Char('a')));
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

} // namespace
