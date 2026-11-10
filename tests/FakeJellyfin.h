// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// An in-process fake Jellyfin server and the helpers shared by the API client tests. Header-only on purpose: every test target includes it (KINEFIN_SOURCE_DIR must be defined for the fixtures)
#pragma once

#include "api/JellyfinClient.h"

#include "FakeSocketConnection.h"

#include <QFile>
#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSet>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrlQuery>
#include <QtTest>

namespace {

// A valid 1x1 PNG, the body of every image route
const QByteArray kTinyPng =
    QByteArray::fromBase64("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwG"
                           "A60e6kgAAAABJRU5ErkJggg==");

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
        listingFixtures.insert(QStringLiteral("Items"), QStringLiteral("items_page"));
        listingFixtures.insert(QStringLiteral("Resume"), QStringLiteral("user_items_resume"));
        listingFixtures.insert(QStringLiteral("Latest"), QStringLiteral("items_latest"));
        listingFixtures.insert(QStringLiteral("NextUp"), QStringLiteral("shows_next_up"));
        listingFixtures.insert(QStringLiteral("Seasons"), QStringLiteral("shows_seasons"));
        listingFixtures.insert(QStringLiteral("Episodes"), QStringLiteral("episodes_page"));
        listingFixtures.insert(QStringLiteral("Similar"), QStringLiteral("items_similar"));
        listingFixtures.insert(QStringLiteral("Genres"), QStringLiteral("genres_page"));
        listingFixtures.insert(QStringLiteral("Filters"), QStringLiteral("items_filters2"));
        listingFixtures.insert(QStringLiteral("Sessions"), QStringLiteral("sessions_list"));
        QObject::connect(&m_server, &QTcpServer::newConnection, &m_server, [this] {
            while (QTcpSocket *socket = m_server.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::readyRead, socket,
                                 [this, socket] { readRequest(socket); });
                QObject::connect(socket, &QTcpSocket::disconnected, socket, [this, socket] {
                    m_buffers.remove(socket);
                    // A later socket may be allocated at this address and must not inherit the WebSocket role
                    m_wsTaken.remove(socket);
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

    // The login response body, with the access token substituted on later successful logins when rotation is on
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
    // Images: GET .../Images/... answers a 1x1 PNG, or imageStatus, or a redirect off the host
    int imageRequests = 0;
    int imageStatus = 200;
    bool redirectImages = false;
    QByteArray imageBody; // overrides the PNG when not empty
    QByteArray lastImageRoute;
    QByteArray lastImageAuth;
    QByteArray lastImageCookie;
    // Every plain HTTP request that was parsed, whatever the route (a refused image request must leave this at 0)
    int requestCount = 0;
    QByteArray lastViewsAuth;
    QByteArray lastLoginAuth;
    // PlaybackInfo: the fixture to answer with, or a status / raw body override
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
    // Playback reports: an ordered log of what arrived, a status for all of them, an optional different status for the first Stopped, and a hold that keeps the answers back until releaseHeld()
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
    // WebSocket upgrade: the attempt is recorded at the header boundary before any accept decision, so tests assert the handshake itself (`Token` only, never `api_key` or a query)
    int upgradeAttempts = 0;
    QString lastSocketRoute;
    QByteArray lastSocketAuth;
    // Everything after the '?' of the upgrade request line; must stay empty
    QString lastSocketQuery;
    // 0 accepts the 101; otherwise the upgrade is answered with this status as plain HTTP
    int socketUpgradeStatus = 0;
    // Keep the TCP connection open but answer nothing: the client-side connect timeout shape
    bool holdSocketUpgrade = false;
    // Login tokens: the fixture one on the first success, a different one on later successes
    bool rotateSecondLoginToken = false;
    QByteArray secondLoginToken = "f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7";
    int successfulLogins = 0;
    // GET /Users/Me (the session re-verification): counts and identity, with a deny switch
    int usersMeRequests = 0;
    QByteArray lastUsersMeAuth;
    bool denyUsersMe = false;

    // The listing routes (fetchItems and family). `holdFirstListings` holds that many next listing replies suppressed until releaseHeld(), `holdListings` holds every one of them, `denyListings` answers 401, `redirectListings` answers 302 off-host. The family (a key of the two last* hashes) is classified from the route fragment, so a test can assert the exact query the client sent. Fixture bodies come from `listingFixtures` (the ctor fills the defaults; a test may override a family)
    int holdFirstListings = 0;
    bool holdListings = false;
    // Families whose requests are held like holdListings does, the others are answered
    QSet<QString> heldFamilies;
    bool denyListings = false;
    bool redirectListings = false;
    QHash<QString, QString> listingFixtures;
    // Raw body per family, overriding the fixture when set (like playbackInfoRawBody)
    QHash<QString, QByteArray> listingRawBody;
    int listingRequests = 0;
    // Status per family answered instead of the body (a 5xx for a failing page)
    QHash<QString, int> listingStatus;
    // When >= 0 the Items family is generated from startIndex/limit: `generatedItemTotal` is the TotalRecordCount, `generatedAvailable` (when >= 0) the rows that really exist, so a page past it comes back short; ids, names and image tags follow the row index
    int generatedItemTotal = -1;
    int generatedAvailable = -1;
    // When >= 0 the Episodes family is generated from startIndex/limit, like generatedItemTotal does for Items: this is the TotalRecordCount
    int generatedEpisodeTotal = -1;
    QList<QByteArray> itemsRoutes; // every Items request in arrival order
    // Items by lower-case id, each a JSON object: when not empty, an Items request with `ids=` answers the entries it finds (an empty envelope when none), whatever the listing fixture says
    QHash<QString, QByteArray> itemsById;
    QHash<QString, QByteArray> lastListingRoute;     // family -> "path?query"
    QHash<QString, QList<QByteArray>> listingRoutes; // family -> every route in arrival order
    QHash<QString, QByteArray> lastListingAuth;      // family -> the Authorization header

    // The most recent accepted WebSocket connection (nullptr until one handshake completes)
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

    // The listing-family fragment of a request route, or "" when it is not one. The specific fragments come first so `/Items/Latest` never reads as the generic /Items
    static QString classifyListingRoute(const QByteArray &route)
    {
        if (route.contains("/UserItems/Resume?"))
            return QStringLiteral("Resume");
        if (route.contains("/Items/Latest?"))
            return QStringLiteral("Latest");
        if (route.contains("/Shows/NextUp?"))
            return QStringLiteral("NextUp");
        if (route.contains("/Seasons?"))
            return QStringLiteral("Seasons");
        if (route.contains("/Episodes?"))
            return QStringLiteral("Episodes");
        if (route.contains("/Similar?"))
            return QStringLiteral("Similar");
        if (route.contains("/Genres?"))
            return QStringLiteral("Genres");
        if (route.contains("/Filters2?"))
            return QStringLiteral("Filters");
        if (route.endsWith("/Sessions") || route.contains("/Sessions?"))
            return QStringLiteral("Sessions");
        if (route.contains("/Items?"))
            return QStringLiteral("Items");
        return {};
    }

    // The full query of a captured listing request: tests dissect it with QUrlQuery
    static QUrlQuery capturedListingQuery(const QByteArray &route)
    {
        QByteArray full = QByteArrayLiteral("http://127.0.0.1");
        full += route;
        return QUrlQuery(QUrl::fromEncoded(full));
    }

  private:
    QByteArray generatedEpisodesBody(const QByteArray &route) const
    {
        const QUrlQuery query = capturedListingQuery(route);
        const int start = query.queryItemValue(QStringLiteral("startIndex")).toInt();
        const int limit = query.queryItemValue(QStringLiteral("limit")).toInt();
        QJsonArray items;
        for (int row = start; row < start + limit && row < generatedEpisodeTotal; ++row) {
            items.append(QJsonObject{
                {QStringLiteral("Id"), QStringLiteral("%1").arg(row + 1, 32, 16, QLatin1Char('e'))},
                {QStringLiteral("Name"), QStringLiteral("Episode %1").arg(row + 1)},
                {QStringLiteral("Type"), QStringLiteral("Episode")},
                {QStringLiteral("SeriesId"), QStringLiteral("aa11bb22cc33dd44ee55ff6677889900")},
                {QStringLiteral("ParentIndexNumber"), 1},
                {QStringLiteral("IndexNumber"), row + 1},
                {QStringLiteral("RunTimeTicks"), qint64(18'000'000'000)},
                {QStringLiteral("ImageTags"),
                 QJsonObject{{QStringLiteral("Primary"), QStringLiteral("ep%1").arg(row)}}}});
        }
        return QJsonDocument(
                   QJsonObject{{QStringLiteral("Items"), items},
                               {QStringLiteral("StartIndex"), start},
                               {QStringLiteral("TotalRecordCount"), generatedEpisodeTotal}})
            .toJson(QJsonDocument::Compact);
    }

    QByteArray itemsByIdBody(const QByteArray &route) const
    {
        const QStringList ids = capturedListingQuery(route)
                                    .queryItemValue(QStringLiteral("ids"))
                                    .split(QLatin1Char(','));
        QJsonArray items;
        for (const QString &id : ids) {
            const QByteArray raw = itemsById.value(id.toLower());
            if (!raw.isEmpty())
                items.append(QJsonDocument::fromJson(raw).object());
        }
        return QJsonDocument(QJsonObject{{QStringLiteral("Items"), items},
                                         {QStringLiteral("StartIndex"), 0},
                                         {QStringLiteral("TotalRecordCount"), items.size()}})
            .toJson(QJsonDocument::Compact);
    }

    QByteArray generatedItemsBody(const QByteArray &route) const
    {
        const QUrlQuery query(QUrl(QString::fromUtf8(route)));
        const int start = query.queryItemValue(QStringLiteral("startIndex")).toInt();
        const int limit = query.queryItemValue(QStringLiteral("limit")).toInt();
        const int available = generatedAvailable >= 0 ? generatedAvailable : generatedItemTotal;
        QJsonArray items;
        for (int row = start; row < start + limit && row < available; ++row) {
            // A 32-digit hex id built from the row index
            items.append(QJsonObject{
                {QStringLiteral("Id"), QStringLiteral("%1").arg(row + 1, 32, 16, QLatin1Char('0'))},
                {QStringLiteral("Name"), QStringLiteral("Item %1").arg(row)},
                {QStringLiteral("Type"), QStringLiteral("Movie")},
                {QStringLiteral("ProductionYear"), 2000 + row % 25},
                {QStringLiteral("ImageTags"),
                 QJsonObject{{QStringLiteral("Primary"), QStringLiteral("tag%1").arg(row)}}}});
        }
        return QJsonDocument(QJsonObject{{QStringLiteral("Items"), items},
                                         {QStringLiteral("StartIndex"), start},
                                         {QStringLiteral("TotalRecordCount"), generatedItemTotal}})
            .toJson(QJsonDocument::Compact);
    }

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
        // Once a WebSocket handshake is accepted, the bytes belong to the frame parser
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
        QByteArray cookie;
        QByteArray contentType;
        QByteArray websocketKey;
        bool upgradeRequest = false;
        for (const QByteArray &raw : lines) {
            const QByteArray line = raw.trimmed();
            if (line.toLower().startsWith("content-length:"))
                contentLength = line.mid(15).trimmed().toInt();
            if (line.toLower().startsWith("authorization:"))
                authorization = line.mid(14).trimmed();
            if (line.toLower().startsWith("cookie:"))
                cookie = line.mid(7).trimmed();
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
        ++requestCount;
        if (upgradeRequest) {
            // Guard first so bytes arriving while the handshake is written can never leak back into the HTTP parser
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
        // The listing routes classify by fragment; `listingFamily` keeps the order so the more specific fragments win (/Items/Latest before /Items, /Sessions before the POST-only playback routes never match a GET)
        const QString listingFamily = classifyListingRoute(route);
        if (method == "GET" && !listingFamily.isEmpty()) {
            ++listingRequests;
            lastListingRoute.insert(listingFamily, route);
            listingRoutes[listingFamily].append(route);
            lastListingAuth.insert(listingFamily, authorization);
            if (listingFamily == QLatin1String("Items"))
                itemsRoutes.append(route);
            if (holdListings || heldFamilies.contains(listingFamily) || holdFirstListings-- > 0) {
                m_held.append(socket);
                return;
            }
            if (listingStatus.contains(listingFamily)) {
                respond(socket, listingStatus.value(listingFamily), {});
            } else if (listingFamily == QLatin1String("Episodes") && generatedEpisodeTotal >= 0
                       && !denyListings && !redirectListings) {
                respond(socket, 200, generatedEpisodesBody(route));
            } else if (listingFamily == QLatin1String("Items") && !itemsById.isEmpty()
                       && route.contains("ids=") && !denyListings && !redirectListings) {
                respond(socket, 200, itemsByIdBody(route));
            } else if (listingFamily == QLatin1String("Items") && generatedItemTotal >= 0
                       && !denyListings && !redirectListings) {
                respond(socket, 200, generatedItemsBody(route));
            } else if (redirectListings) {
                respond(socket, 302, {},
                        QByteArrayLiteral("Location: https://unexpected.invalid/") + route.mid(1)
                            + QByteArrayLiteral("\r\n"));
            } else if (denyListings) {
                respond(socket, 401, {});
            } else if (listingRawBody.contains(listingFamily)) {
                respond(socket, 200, listingRawBody.value(listingFamily));
            } else {
                respond(socket, 200, fixture(listingFixtures.value(listingFamily)));
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
        if (method == "GET" && route.contains("/Images/")) {
            ++imageRequests;
            lastImageRoute = route;
            lastImageAuth = authorization;
            lastImageCookie = cookie;
            if (redirectImages) {
                respond(socket, 302, {}, "Location: https://unexpected.invalid/image.png\r\n");
            } else if (imageStatus != 200) {
                respond(socket, imageStatus, {});
            } else {
                // Cache-Control mirrors what the server sends for a tagged image
                respond(socket, 200, imageBody.isEmpty() ? kTinyPng : imageBody,
                        "Cache-Control: public, max-age=31536000\r\n");
            }
            return;
        }
        respond(socket, 404, {});
    }

    // The server comes last so it is destroyed first: its child sockets may still emit `disconnected` while it dies, and the slots touch the members above
    QHash<QTcpSocket *, QByteArray> m_buffers;
    QHash<QTcpSocket *, FakeSocketConnection *> m_wsTaken;
    QList<QPointer<QTcpSocket>> m_held;
    QTcpServer m_server;
    QList<FakeSocketConnection *> m_wsConnections;
};

inline bool prepare(Kinefin::JellyfinClient &client, FakeJellyfin &server)
{
    if (!client.setDeviceIdentity(QStringLiteral("Kinefin test"), QStringLiteral("stable-test-id"),
                                  QStringLiteral("0.0.1")))
        return false;
    return client.connectToServer(server.url(QStringLiteral("/jellyfin")));
}

const QString kPlayItem = QStringLiteral("c7dd3b1ac7dd3b1a9d42f2ab74f8a1b9");
// The access token of tests/fixtures/authenticate_success.json
const QByteArray kFixtureToken = "f2f4b7c1ee8a4d0bb2a5f9d1f9e0e2ab";
const QByteArray kTokenField = QByteArray("Token=\"") + kFixtureToken + QByteArray("\"");

// Connects and logs in; QTRY_* macros cannot live in a helper that returns a value
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
    if (!client.authenticate(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"))) {
        qWarning() << "login: authenticate refused by the client";
        return false;
    }
    if (!QTest::qWaitFor([&] { return loggedIn.size() == 1; }, 5000)) {
        qWarning() << "login: loginSucceeded never arrived; authenticated"
                   << client.isAuthenticated()
                   << "logins seen by the fake:" << server.successfulLogins;
        return false;
    }
    return true;
}

// A valid direct-play response plus a padding field: the only thing wrong with it is its size
inline QByteArray paddedPlaybackInfo(int paddingBytes)
{
    QJsonObject object =
        QJsonDocument::fromJson(fixture(QStringLiteral("playbackinfo_direct"))).object();
    object.insert(QStringLiteral("Padding"), QString(paddingBytes, QLatin1Char('a')));
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

} // namespace
