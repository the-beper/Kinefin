// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "MpvProbe.h"

#include "api/DeviceProfile.h"
#include "api/JellyfinClient.h"
#include "api/JellyfinSocketClient.h"
#include "api/PlaybackReport.h"
#include "models/PlayQueue.h"
#include "player/ClipExporter.h"
#include "player/PlaybackEngine.h"
#include "player/PlaybackReporter.h"
#include "player/PlaybackSession.h"
#include "player/PlayerController.h"
#include "settings/ClientSettings.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrlQuery>
#include <QUuid>
#include <QWebSocket>
#include <QWebSocketProtocol>
#include <QtTest>

// Opt-in live checks against a real Jellyfin server: the websocket live steps and the reporting live checks. Every slot SKIPs unless the environment provides the server, so `ctest` under check.sh runs this file as a plain skip everywhere else. Bring: KINEFIN_LIVE_BASE_URL (e.g. http://192.168.1.10:8096), KINEFIN_LIVE_USER, KINEFIN_LIVE_PASSWORD. The sessionsRoleVisibility slot additionally takes, when an admin account may be used for the comparison: KINEFIN_LIVE_ADMIN_USER/PASSWORD. The access token never leaves the process: prints carry names, ids, routes, statuses and raw frames only, and every login uses its own throw-away device id
class TestJellyfinLive : public QObject {
    Q_OBJECT

  public:
    TestJellyfinLive()
        : m_base(QUrl(qEnvironmentVariable("KINEFIN_LIVE_BASE_URL"))),
          m_user(qEnvironmentVariable("KINEFIN_LIVE_USER")),
          m_password(qEnvironmentVariable("KINEFIN_LIVE_PASSWORD")),
          m_live(m_base.isValid() && !m_base.isEmpty() && !m_user.isEmpty()
                 && !m_password.isEmpty()),
          m_adminUser(qEnvironmentVariable("KINEFIN_LIVE_ADMIN_USER")),
          m_adminPassword(qEnvironmentVariable("KINEFIN_LIVE_ADMIN_PASSWORD")),
          m_adminLive(!m_adminUser.isEmpty() && !m_adminPassword.isEmpty())
    {
    }

  private Q_SLOTS:
    // REST round trip plus the session identity. ~2 s
    void authAndSessionSmoke();

    // ~75 s: the QWebSocket handshake with the component's own request, the exact ForceKeepAlive the server opens with, the client's KeepAlive answer, and a hold past the 60 s close deadline
    void handshakeAndKeepAliveCycle();

    // The component's full heartbeat cycle (~210 s): authenticated once, then 200 s of pure waiting with the socket confident the whole time
    void componentSurvivesThreeMinutes();

    // ~70 s: during 61 s of client silence the server announces ForceKeepAlive again, between 45 and 60 s
    void forceKeepAliveOnSilence();

    // ~25 s: with the SessionsStart interest out, Sessions frames arrive (or are proven not to). Either outcome is recorded verbatim
    void sessionsWhileSubscribed();

    // ~25 s: a genuine second session of the same user drives Play, a Playstate and a GeneralCommand at this device's session; the components route them
    void commandsFromSecondSession();

    // The raw byte form of the remote-control frames: a direct WebSocket with a capture socket, the controller drives it from a separate device. ~20 s
    void rawCommandShapes();

    // The frames a scan wake-up and a user-data change look like on the wire (this user must hold the LibraryScan task trigger; the marking is undone at the end)
    void rawScanAndUserDataShapes();

    // On 12.4 the token logout (POST /Sessions/Logout, 204) ends the authenticated socket; a fresh handshake with the dead token is refused and /Users/Me answers 401. The server-side admin device deletion was not reachable (404)
    void tokenLogoutClosesEverything();

    // Reporting, ~10 s: the pure report bodies the client builder produces, posted by hand and the server's real answers observed, including a duplicate Stopped
    void reportsAnswer204();

    // Reporting, ~10 s: a Failed Stopped leaves the user's play state untouched
    void failedStopsLeaveUserDataAlone();

    // ~15 s: an episode marked watched by one client is read by another through refreshUserData, and the series of the episode comes with it (the marking is undone at the end)
    void userDataRefreshReadsWhatAnotherSessionWrote();

    // Reporting, ~25 s: a 4K HDR item forced through the HLS negotiation, a Transcode marker in the live session record, and a Stopped that ends the server job
    void hlsTranscodeLifecycle();

    // Engine, ~40 s: a direct-playable movie under the default profile, the engine plays the real stream, the server session record carries it, pause/resume/seek/stop run, and the Stopped clears the play
    void engineDirectPlayLifecycle();

    // Engine, ~45 s: an HDR item forced to transcode, mpv plays the negotiated HLS stream, and the Stopped tears the server job down
    void engineTranscodeLifecycle();

    // Engine, ~60 s: >20 s of held-pause reporting, then stop; the server's user-data record keeps the continue point
    void engineProgressAdvancesUserData();

    // Clip export from a transcode (~60 s): a movie capped into a transcode plays 25 s, the last 20 s are marked as an A-B range, `ab-loop-align-cache` and `ab-loop-dump-cache` write what mpv holds, and the time the two commands block the player and the distance of the aligned start from the mark are the facts; the dump is then encoded by ClipExporter from the mark on
    void transcodeCacheDump();

    // Clip of a transcode through the player (~90 s): a real PlayerController plays a movie, a quality step makes the server transcode it, the marks take 6 s just played, the clip is encoded from mpv's cache into the granted file, and the profile's clips folder is left empty
    void transcodeClipThroughThePlayer();

    // Engine, ~50 s: a byte-forwarding local proxy carries client and stream; cutting it mid-play gives exactly one Failed->Stopping->Idle path, no second play identity, and re-authentication on the real base recovers
    void engineInterruptionSeversAndRecovers();

    // Adaptive bitrate fallback (~2-4 min): a real PlayerController plays a direct-playable movie of at least 5 Mbit/s through a local proxy, the proxy is throttled to 40 % of the file's bitrate, and the controller must lower the quality on its own: a transcode at a lower bitrate, one Stopped then one Start, Playing again near the same position, and a Stopped that clears the server's play
    void playerFallsBackOnAThrottledLink();

    // Engine, ~40 s: local-logout scope (a server-driven expiry needs an admin action this harness cannot reach). The engine keeps playing through a cleared credential, the stop needs no server, and no new playback identity forms
    void engineSurvivesALocalLogout();

    // BaseUrl handshake round (~15 s): the client's whole loop (probe, login, WebSocket handshake, one report) through a forwarder that serves the server under a path prefix, standing in for upstream's `<host>/jellyfin` deployments
    void subPathBaseUrlRound();

    // Endpoint-matrix round, auth wire format (~10 s): a raw login carries the exact MediaBrowser header; the echoed token then rides /Users/Me as the full MediaBrowser header (200), and the legacy variants (X-Emby-Token, ?api_key=, ?ApiKey=) are recorded whatever they answer
    void authHeaderWireEcho();

    // Endpoint-matrix round, listings (~60 s): every browse/search/session fetcher of the component against the real server (compose, ids tunnel, home rows, series navigation, similar incl. the Episode/unknown-id edges, genres, Filters2, sessions), the token-free image route unauthenticated, and the server's echo of an invalid query member
    void browseListingsLive();

    // Endpoint-matrix round, /Sessions roles (~15 s): the visible set as admin vs non-admin, plus the non-admin substitution of a foreign controllableByUserId. Skips without KINEFIN_LIVE_ADMIN_*
    void sessionsRoleVisibility();

    // Endpoint-matrix round, trickplay (~15 s): the item's Trickplay field, the tiles.m3u8 ladder and one tile jpg's real dimensions. Skips when no trickplay scan exists on the server
    void trickplayLayoutProbe();

    // Endpoint-matrix round, PlaybackInfo defaults (~30 s): the body without a bitrate ceiling vs an explicit low one vs no DeviceProfile at all — the TranscodingUrl bitrate the server picks is the fact
    void playbackInfoDefaultsProbe();

    // Embedded text subtitles in a transcode (~20 s): a movie MKV with embedded SubRip streams, PlaybackInfo with Kinefin's device profile under a cap that forces a transcode; how the server delivers each embedded text stream (method, URL shape, query) and what the client's parser keeps are the facts
    void subtitleDeliveryProbe();

    // Playlist editing (~10 s): a private playlist of three movies, the first entry moved to newIndex 2 and the order read back (what newIndex means), the owner's CanDelete on GET /Items/{id}, then the delete through the client. The playlist is removed even when a check fails
    void playlistEdits();

    // The SyncPlay protocol (~90 s): two sessions of the user, each with its own socket, through New, List, Join (without a socket, then with one but no media control), SetNewQueue, Ping, Ready, Pause, Seek, a Ready off position, Unpause, Buffering, IgnoreWait, NextItem twice, a socket drop and reopen, the error updates and an item the user cannot see; every frame and status printed (the output pairs group ids with the user's name: keep it out of shared logs). Both sessions leave even when a check fails
    void syncPlayProtocol();

    // The SyncPlay group queue (~40 s): two sessions, a queue of three movies, NextItem with an entry after it (twice), PreviousItem, SetPlaylistItem, Queue and QueueNext, MovePlaylistItem, RemoveFromPlaylist (an entry, the playing one, then all), SetRepeatMode and SetShuffleMode, NextItem at the end under RepeatAll; every frame and status printed (ids next to the user's name: keep the output out of shared logs). Both sessions leave even when a check fails
    void syncPlayQueueProtocol();

  private:
    int deviceSequence = 0;
    static constexpr auto LIVE_FACT = "live-fact:";
    QString sourceDeviceId() const { return m_deviceId; }
    bool authenticate(Kinefin::JellyfinClient &client);
    bool authenticateAs(Kinefin::JellyfinClient &client, const QString &user,
                        const QString &password);
    void recordFrame(const QString &frame);
    bool frameSeen(QWebSocket *socket, qint64 ms, qsizetype from, QString *rawMessage,
                   const QByteArray &needle);
    QWebSocket *openSocket(Kinefin::JellyfinClient &client);
    bool socketFirstExchange(QWebSocket *socket);
    QJsonDocument getDocument(Kinefin::JellyfinClient &client, const QString &path);
    QJsonObject getJson(Kinefin::JellyfinClient &client, const QString &path);
    int postJson(Kinefin::JellyfinClient &client, const QString &path, const QJsonObject &body,
                 QByteArray *answer = nullptr);
    int deleteRequest(Kinefin::JellyfinClient &client, const QString &path);
    // Raw GET of any content type with the client's identity (or without it, for the token-free routes and the negative credential probes): the status and the body out, and the Content-Type when asked for
    int probeRaw(Kinefin::JellyfinClient &client, const QString &path, QByteArray *body,
                 bool withAuth, QString *contentType = nullptr);
    QString firstItemId(Kinefin::JellyfinClient &client);
    QString ownSessionId(Kinefin::JellyfinClient &client, const QString &ownDeviceId);
    QJsonObject fetchMe(Kinefin::JellyfinClient &client);
    int postReport(Kinefin::JellyfinClient &client, const Kinefin::PlaybackReport &report);
    static void fill(Kinefin::PlaybackReport &report, const QString &itemId,
                     const QString &playSessionId, qint64 positionTicks);
    QJsonObject itemUserData(Kinefin::JellyfinClient &client, const QString &userId,
                             const QString &itemId);
    QString sessionSnapshot(Kinefin::JellyfinClient &client, const QString &deviceId,
                            const QString &playSessionId);
    // Engine live gate round helpers
    QString hdrItemId(Kinefin::JellyfinClient &client);
    bool findDirectPlayable(Kinefin::JellyfinClient &client, QString *itemId,
                            Kinefin::PlaybackSource *source, qint64 minBitrate = 0);
    void assertTokenFreeMediaUrl(const QUrl &url);
    void recordAndAssertPlaying(Kinefin::JellyfinClient &client, const QString &deviceId,
                                const Kinefin::PlaybackSource &source,
                                Kinefin::PlaybackSession &session, Kinefin::PlaybackEngine *engine);

    QUrl m_base;
    QString m_user;
    QString m_password;
    bool m_live = false;
    QString m_adminUser;
    QString m_adminPassword;
    bool m_adminLive = false;
    qsizetype m_count = 0;
    QStringList m_frames;
    QString m_deviceId;
};

int deviceSequence = 0;

// One authenticated client per use; every device id unique so /Sessions entries never mix
bool TestJellyfinLive::authenticate(Kinefin::JellyfinClient &client)
{
    return authenticateAs(client, m_user, m_password);
}

bool TestJellyfinLive::authenticateAs(Kinefin::JellyfinClient &client, const QString &user,
                                      const QString &password)
{
    ++deviceSequence;
    const QString deviceId = QStringLiteral("kinefin-live-fiddle-%1").arg(deviceSequence);
    m_deviceId = deviceId;
    if (!client.setDeviceIdentity(QStringLiteral("Kinefin"), deviceId, QStringLiteral("0.0.1"))) {
        qWarning() << "setDeviceIdentity refused";
        return false;
    }
    if (!client.connectToServer(m_base)) {
        qWarning() << "connectToServer refused for" << m_base;
        return false;
    }
    // The opt-in is scoped to the current server and `connectToServer` resets it: set it now, while the transport is connecting, so the credentials may travel over plain LAN http
    client.setAllowInsecureHttp(true);
    QSignalSpy ready(&client, &Kinefin::JellyfinClient::serverReady);
    if (!ready.wait(15000)) {
        qWarning() << "the server probe never answered";
        return false;
    }
    QSignalSpy loggedIn(&client, &Kinefin::JellyfinClient::loginSucceeded);
    if (!client.authenticate(user, password)) {
        qWarning() << "authenticate call refused (client-side)";
        return false;
    }
    return loggedIn.wait(15000);
}

void TestJellyfinLive::recordFrame(const QString &frame)
{
    m_frames.append(frame);
}

// Whether a frame containing `needle` arrives within `ms` after index `from`; the message is copied out
bool TestJellyfinLive::frameSeen(QWebSocket *socket, qint64 ms, qsizetype from, QString *rawMessage,
                                 const QByteArray &needle)
{
    Q_UNUSED(socket)
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QTest::qWait(100);
        for (qsizetype i = from; i < m_frames.size(); ++i) {
            if (m_frames.at(i).contains(QString::fromUtf8(needle))) {
                if (rawMessage)
                    *rawMessage = m_frames.at(i);
                return true;
            }
        }
    }
    return false;
}

// Opens the upgrade; nullptr when the handshake is refused
QWebSocket *TestJellyfinLive::openSocket(Kinefin::JellyfinClient &client)
{
    auto *socket = new QWebSocket(QStringLiteral("Kinefin live check"),
                                  QWebSocketProtocol::VersionLatest, this);
    QSignalSpy connected(socket, &QWebSocket::connected);
    connect(socket, &QWebSocket::textMessageReceived, this,
            [this](const QString &frame) { recordFrame(frame); });
    socket->open(client.webSocketRequest());
    if (!connected.wait(15000))
        return nullptr;
    return socket;
}

// The QWebSocket handshake with the component's own request: 101 answer, then the first server frame (ForceKeepAlive) inside the first seconds
bool TestJellyfinLive::socketFirstExchange(QWebSocket *socket)
{
    QString raw;
    const bool seen = frameSeen(socket, 15000, 0, &raw, QByteArrayLiteral("ForceKeepAlive"));
    if (seen)
        qInfo() << LIVE_FACT << "first ForceKeepAlive raw:" << raw;
    return seen;
}

// The skip guard lives in every slot itself; QSKIP returns from its caller
void TestJellyfinLive::authAndSessionSmoke()
{
    if (!m_live)
        QSKIP("no KINEFIN_LIVE_* environment, skipping the live checks");
    qInfo() << "base:" << m_base << "user:" << m_user;
    Kinefin::JellyfinClient client;
    QVERIFY(authenticate(client));
    QVERIFY(client.isAuthenticated());
    QVERIFY(!client.serverId().isEmpty());
    QVERIFY(!client.userId().isEmpty());
    QVERIFY(!client.webSocketRequest().url().isEmpty());
    client.disconnectFromServer();
}

void TestJellyfinLive::componentSurvivesThreeMinutes()
{
    if (!m_live)
        QSKIP("no KINEFIN_LIVE_* environment, skipping the live checks");
    Kinefin::JellyfinClient client;
    Kinefin::JellyfinSocketClient socket(&client);
    QVERIFY(authenticate(client));
    // Every heartbeat is answered inside the component; the state stays stable while nothing local touches it
    QTest::qWait(200000);
    QCOMPARE(socket.state(), Kinefin::JellyfinSocketClient::State::Authenticated);
    client.disconnectFromServer();
}

void TestJellyfinLive::handshakeAndKeepAliveCycle()
{
    if (!m_live)
        QSKIP("no KINEFIN_LIVE_* environment, skipping the live checks");
    Kinefin::JellyfinClient client;
    QVERIFY(authenticate(client));
    QWebSocket *socket = openSocket(client);
    QVERIFY2(socket, "the QWebSocket handshake did not reach 101");
    QVERIFY(socketFirstExchange(socket));
    qInfo() << LIVE_FACT << "ForceKeepAlive raw:" << m_frames.constFirst();
    socket->sendTextMessage(QStringLiteral("{\"MessageType\":\"KeepAlive\"}"));
    QTest::qWait(70000);
    QVERIFY2(socket->isValid(), "the socket did not survive 70 s after the KeepAlive answer");
    socket->close(QWebSocketProtocol::CloseCodeNormal);
    client.disconnectFromServer();
}

void TestJellyfinLive::forceKeepAliveOnSilence()
{
    if (!m_live)
        QSKIP("no KINEFIN_LIVE_* environment, skipping the live checks");
    Kinefin::JellyfinClient client;
    QVERIFY(authenticate(client));
    QWebSocket *socket = openSocket(client);
    QVERIFY2(socket, "the QWebSocket handshake did not reach 101");
    QVERIFY(socketFirstExchange(socket));
    const qsizetype base = m_frames.count();
    QElapsedTimer silence;
    silence.start();
    QString secondFrame;
    while (silence.elapsed() < 62000) {
        QTest::qWait(1000);
        for (qsizetype i = base; i < m_frames.count(); ++i) {
            if (m_frames.at(i).contains(
                    QString::fromUtf8(QByteArrayLiteral("ForceKeepAlive").constData())))
                secondFrame = m_frames.at(i);
        }
        if (!secondFrame.isEmpty())
            break;
    }
    QVERIFY2(!secondFrame.isEmpty(), "no second ForceKeepAlive during 62 s of silence");
    qInfo() << LIVE_FACT << "silent ForceKeepAlive after" << silence.elapsed()
            << "ms raw:" << secondFrame;
    QVERIFY2(silence.elapsed() >= 43000 && silence.elapsed() <= 62500,
             "the silence-driven ForceKeepAlive fell outside [45, 60] s");
    socket->close(QWebSocketProtocol::CloseCodeNormal);
    client.disconnectFromServer();
}

void TestJellyfinLive::sessionsWhileSubscribed()
{
    if (!m_live)
        QSKIP("no KINEFIN_LIVE_* environment, skipping the live checks");
    Kinefin::JellyfinClient client;
    Kinefin::JellyfinSocketClient socket(&client);
    QVERIFY(authenticate(client));
    QTRY_COMPARE(socket.state(), Kinefin::JellyfinSocketClient::State::Authenticated);
    QSignalSpy sessions(&socket, &Kinefin::JellyfinSocketClient::sessionsUpdated);
    socket.setSessionsSubscription(true);
    QTest::qWait(20000);
    qInfo() << LIVE_FACT << "Sessions frames while subscribed:" << sessions.count();
    if (sessions.count() >= 1)
        qInfo() << LIVE_FACT << "Sessions array size in the first frame:"
                << sessions.first().first().value<QJsonArray>().size();
    client.disconnectFromServer();
}

// Aux REST: a GET with the client's own Authorization header; the URL carries no token. The body may be an object or an array (12.4's /Sessions answers a bare array)
QJsonDocument TestJellyfinLive::getDocument(Kinefin::JellyfinClient &client, const QString &path)
{
    QNetworkAccessManager network;
    QUrl url = client.serverUrl();
    // QUrl::setPath would percent-encode a '?' inside the path: split the query off
    const qsizetype queryStart = path.indexOf(QLatin1Char('?'));
    url.setPath(url.path() + (queryStart < 0 ? path : path.left(queryStart)));
    if (queryStart >= 0)
        url.setQuery(QUrlQuery(path.mid(queryStart + 1)));
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setRawHeader(QByteArrayLiteral("Accept"), QByteArrayLiteral("application/json"));
    const QByteArray token = client.streamAuthorization();
    if (!token.isEmpty())
        request.setRawHeader(QByteArrayLiteral("Authorization"), token);
    QNetworkReply *reply = network.get(request);
    QSignalSpy done(reply, &QNetworkReply::finished);
    done.wait(15000);
    QJsonDocument result;
    if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200)
        result = QJsonDocument::fromJson(reply->readAll());
    else
        qWarning() << "the aux GET for" << path << "answered"
                   << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    reply->deleteLater();
    return result;
}

// Aux REST: a DELETE with the client's own Authorization header; the answer status only
int TestJellyfinLive::deleteRequest(Kinefin::JellyfinClient &client, const QString &path)
{
    QNetworkAccessManager network;
    QUrl url = client.serverUrl();
    const qsizetype queryStart = path.indexOf(QLatin1Char('?'));
    url.setPath(url.path() + (queryStart < 0 ? path : path.left(queryStart)));
    if (queryStart >= 0)
        url.setQuery(QUrlQuery(path.mid(queryStart + 1)));
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setRawHeader(QByteArrayLiteral("Accept"), QByteArrayLiteral("application/json"));
    const QByteArray token = client.streamAuthorization();
    if (!token.isEmpty())
        request.setRawHeader(QByteArrayLiteral("Authorization"), token);
    QNetworkReply *reply = network.deleteResource(request);
    QSignalSpy done(reply, &QNetworkReply::finished);
    done.wait(15000);
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    reply->deleteLater();
    return status;
}

QJsonObject TestJellyfinLive::getJson(Kinefin::JellyfinClient &client, const QString &path)
{
    return getDocument(client, path).object();
}

// Raw GET of any content type: the status out, the body captured; `withAuth` false probes the token-free routes (images) and the negative credential cases
int TestJellyfinLive::probeRaw(Kinefin::JellyfinClient &client, const QString &path,
                               QByteArray *body, bool withAuth, QString *contentType)
{
    QNetworkAccessManager network;
    QUrl url = client.serverUrl();
    const qsizetype queryStart = path.indexOf(QLatin1Char('?'));
    url.setPath(url.path() + (queryStart < 0 ? path : path.left(queryStart)));
    if (queryStart >= 0)
        url.setQuery(QUrlQuery(path.mid(queryStart + 1)));
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    if (withAuth) {
        const QByteArray token = client.streamAuthorization();
        if (!token.isEmpty())
            request.setRawHeader(QByteArrayLiteral("Authorization"), token);
    }
    QNetworkReply *reply = network.get(request);
    QSignalSpy done(reply, &QNetworkReply::finished);
    done.wait(15000);
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (body)
        *body = reply->readAll();
    if (contentType)
        *contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString();
    reply->deleteLater();
    return status;
}

// Aux REST: a POST with the client's identity header; the answer status, and the body when asked for
int TestJellyfinLive::postJson(Kinefin::JellyfinClient &client, const QString &path,
                               const QJsonObject &body, QByteArray *answer)
{
    QNetworkAccessManager network;
    QUrl url = client.serverUrl();
    // Same split as the GET helper: a path with a query would percent-encode the '?'
    const qsizetype queryStart = path.indexOf(QLatin1Char('?'));
    url.setPath(url.path() + (queryStart < 0 ? path : path.left(queryStart)));
    if (queryStart >= 0)
        url.setQuery(QUrlQuery(path.mid(queryStart + 1)));
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setRawHeader(QByteArrayLiteral("Accept"), QByteArrayLiteral("application/json"));
    request.setRawHeader(QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json"));
    const QByteArray token = client.streamAuthorization();
    if (!token.isEmpty())
        request.setRawHeader(QByteArrayLiteral("Authorization"), token);
    QNetworkReply *reply =
        network.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    QSignalSpy done(reply, &QNetworkReply::finished);
    done.wait(15000);
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (answer)
        *answer = reply->readAll();
    reply->deleteLater();
    return status;
}

QString TestJellyfinLive::firstItemId(Kinefin::JellyfinClient &client)
{
    const QJsonObject items = getJson(
        client, QStringLiteral("/Items?IncludeItemTypes=Movie&Recursive=true&SortBy=Name&Limit=1"));
    const QJsonArray found = items.value(QStringLiteral("Items")).toArray();
    if (found.isEmpty()) {
        qWarning() << "the library probe answered: keys" << items.keys();
        return {};
    }
    return found.first().toObject().value(QStringLiteral("Id")).toString();
}

// The /Sessions entry (key `Id` — Live fact: 12.4 has no `SessionId` key there) of the given device; remote commands target it
QString TestJellyfinLive::ownSessionId(Kinefin::JellyfinClient &client, const QString &ownDeviceId)
{
    const QJsonArray sessions = getDocument(client, QStringLiteral("/Sessions")).array();
    for (const auto &value : sessions) {
        const QJsonObject session = value.toObject();
        qWarning() << "/Sessions entry device" << session.value(QStringLiteral("DeviceId"))
                   << "active" << session.value(QStringLiteral("IsActive")).toBool() << "id"
                   << session.value(QStringLiteral("Id")).toString();
        if (session.value(QStringLiteral("DeviceId")).toString() == ownDeviceId)
            return session.value(QStringLiteral("Id")).toString();
    }
    qWarning() << "no /Sessions entry for device" << ownDeviceId;
    return {};
}

QJsonObject TestJellyfinLive::fetchMe(Kinefin::JellyfinClient &client)
{
    return getJson(client, QStringLiteral("/Users/Me"));
}

void TestJellyfinLive::commandsFromSecondSession()
{
    if (!m_live)
        QSKIP("no KINEFIN_LIVE_* environment, skipping the live checks");
    Kinefin::JellyfinClient watched;
    Kinefin::JellyfinClient controller;             // a separate device for remote commands
    Kinefin::JellyfinSocketClient socket(&watched); // follows the login below
    QVERIFY(authenticate(watched));
    const QString watchedDeviceId = m_deviceId;
    QTRY_COMPARE(socket.state(), Kinefin::JellyfinSocketClient::State::Authenticated);
    QVERIFY(authenticate(controller));
    const QString sessionId = ownSessionId(watched, watchedDeviceId);
    QVERIFY2(!sessionId.isEmpty(), "no /Sessions entry appeared for this device");

    QSignalSpy played(&socket, &Kinefin::JellyfinSocketClient::playRequested);
    QSignalSpy playstate(&socket, &Kinefin::JellyfinSocketClient::playstateCommand);
    QSignalSpy general(&socket, &Kinefin::JellyfinSocketClient::generalCommand);

    const QString movie = firstItemId(controller);
    QVERIFY2(!movie.isEmpty(), "no movie in the library to send Play with");
    // The 12.4 route carries the play command as query parameters, not in a body: the body variant answers 400 ("playCommand field is required") even when present
    const int playStatus =
        postJson(controller,
                 QStringLiteral("/Sessions/") + sessionId
                     + QStringLiteral("/Playing?playCommand=PlayNow&itemIds=") + movie,
                 {});
    qInfo() << LIVE_FACT << "Play route status:" << playStatus;
    QTRY_VERIFY_WITH_TIMEOUT(played.count() >= 1, 15000);
    const auto request = played.first().first().value<Kinefin::RemotePlayRequest>();
    qInfo() << LIVE_FACT << "Play routed:" << request.command << request.itemIds.count()
            << "ids, ticks" << request.startPositionTicks;

    QCOMPARE(postJson(controller,
                      QStringLiteral("/Sessions/") + sessionId + QStringLiteral("/Playing/Pause"),
                      {}),
             204);
    QTRY_VERIFY_WITH_TIMEOUT(playstate.count() >= 1, 15000);
    qInfo() << LIVE_FACT << "Playstate routed:" << playstate.first().at(0).toString()
            << "ticks sentinel" << playstate.first().at(1).toLongLong();

    const QJsonObject commandBody{
        {QStringLiteral("Name"), QStringLiteral("DisplayMessage")},
        {QStringLiteral("Arguments"),
         QJsonObject{{QStringLiteral("Header"), QStringLiteral("Kinefin live check")},
                     {QStringLiteral("Message"), QStringLiteral("round trip")}}}};
    QCOMPARE(postJson(controller,
                      QStringLiteral("/Sessions/") + sessionId + QStringLiteral("/Command"),
                      commandBody),
             204);
    QTRY_VERIFY_WITH_TIMEOUT(general.count() >= 1, 15000);
    qInfo() << LIVE_FACT << "GeneralCommand routed:" << general.first().at(0).toString()
            << "arguments" << general.first().at(1).toMap();
    watched.disconnectFromServer();
    controller.disconnectFromServer();
}

void TestJellyfinLive::rawCommandShapes()
{
    if (!m_live)
        QSKIP("no KINEFIN_LIVE_* environment, skipping the live checks");
    Kinefin::JellyfinClient watched;
    Kinefin::JellyfinClient controller;
    QVERIFY(authenticate(watched));
    const QString watchedDeviceId = m_deviceId;
    QWebSocket *socket = openSocket(watched);
    QVERIFY2(socket, "the capture handshake did not reach 101");
    QVERIFY(authenticate(controller));
    const QString sessionId = ownSessionId(watched, watchedDeviceId);
    QVERIFY2(!sessionId.isEmpty(), "no /Sessions entry appeared for the capture socket");
    const QString movie = firstItemId(controller);
    QVERIFY2(!movie.isEmpty(), "no movie in the library to send Play with");
    QCOMPARE(postJson(controller,
                      QStringLiteral("/Sessions/") + sessionId + QStringLiteral("/Playing")
                          + QStringLiteral("?playCommand=PlayNow&itemIds=") + movie,
                      {}),
             204);
    QTest::qWait(8000);
    for (const QString &frame : m_frames)
        qInfo() << LIVE_FACT << "raw inbound frame above:" << frame;
    QCOMPARE(postJson(controller,
                      QStringLiteral("/Sessions/") + sessionId + QStringLiteral("/Playing/Pause"),
                      {}),
             204);
    QTest::qWait(4000);
    QCOMPARE(
        postJson(controller, QStringLiteral("/Sessions/") + sessionId + QStringLiteral("/Command"),
                 QJsonObject{
                     {QStringLiteral("Name"), QStringLiteral("DisplayMessage")},
                     {QStringLiteral("Arguments"),
                      QJsonObject{{QStringLiteral("Header"), QStringLiteral("Kinefin live check")},
                                  {QStringLiteral("Message"), QStringLiteral("round trip")}}}}),
        204);
    QTest::qWait(4000);
    for (const QString &frame : m_frames)
        qInfo() << LIVE_FACT << "raw inbound frame (2):" << frame;
    socket->close(QWebSocketProtocol::CloseCodeNormal);
    watched.disconnectFromServer();
    controller.disconnectFromServer();
}

// Posts a report with the exact body the pure builder produces; the answer status observed
int TestJellyfinLive::postReport(Kinefin::JellyfinClient &client,
                                 const Kinefin::PlaybackReport &report)
{
    const std::optional<QJsonObject> body = Kinefin::playbackReportBody(report);
    if (!body)
        return -1;
    QString route;
    switch (report.kind) {
    case Kinefin::PlaybackReport::Kind::Start:
        route = QStringLiteral("/Sessions/Playing");
        break;
    case Kinefin::PlaybackReport::Kind::Progress:
        route = QStringLiteral("/Sessions/Playing/Progress");
        break;
    case Kinefin::PlaybackReport::Kind::Stopped:
        break;
    }
    if (route.isEmpty())
        route = QStringLiteral("/Sessions/Playing/Stopped");
    return postJson(client, route, *body);
}

void TestJellyfinLive::fill(Kinefin::PlaybackReport &report, const QString &itemId,
                            const QString &playSessionId, qint64 positionTicks)
{
    report.itemId = itemId;
    report.mediaSourceId = itemId;
    report.playSessionId = playSessionId;
    report.positionTicks = positionTicks;
    report.method = Kinefin::PlaybackSource::Method::Direct;
}

// The user-data record of one item through the 12.4 route (`/Users/{id}/Items/{id}/UserData`)
QJsonObject TestJellyfinLive::itemUserData(Kinefin::JellyfinClient &client, const QString &userId,
                                           const QString &itemId)
{
    return getJson(client, QStringLiteral("/Users/") + userId + QStringLiteral("/Items/") + itemId
                               + QStringLiteral("/UserData"));
}

// The /Sessions record of `deviceId` as 12.4 presents it (the view has no PlaySessionId key at all — the live fact; the play session is only reported, never read back)
QString TestJellyfinLive::sessionSnapshot(Kinefin::JellyfinClient &client, const QString &deviceId,
                                          const QString &playSessionId)
{
    Q_UNUSED(playSessionId)
    const QJsonArray sessions = getDocument(client, QStringLiteral("/Sessions")).array();
    for (const auto &value : sessions) {
        const QJsonObject session = value.toObject();
        if (session.value(QStringLiteral("DeviceId")).toString() != deviceId)
            continue;
        const QJsonObject nowPlaying = session.value(QStringLiteral("NowPlayingItem")).toObject();
        const QJsonObject compact{
            {QStringLiteral("nowPlayingType"), nowPlaying.value(QStringLiteral("Type")).toString()},
            {QStringLiteral("nowPlayingItem"), nowPlaying.value(QStringLiteral("Id")).toString()},
            {QStringLiteral("positionTicks"), session.value(QStringLiteral("PlayState"))
                                                  .toObject()
                                                  .value(QStringLiteral("PositionTicks"))
                                                  .toInt()},
        };
        return QString::fromUtf8(QJsonDocument(compact).toJson(QJsonDocument::Compact));
    }
    return QStringLiteral("(no /Sessions entry)");
}

void TestJellyfinLive::reportsAnswer204()
{
    if (!m_live)
        QSKIP("no KINEFIN_LIVE_* environment, skipping the live checks");
    Kinefin::JellyfinClient client;
    QVERIFY(authenticate(client));
    const QString movie = firstItemId(client);
    QVERIFY2(!movie.isEmpty(), "no movie in the library to report on");
    const QString playSessionId = QStringLiteral("kinefin-live-fiddle-psid-r1");
    Kinefin::PlaybackReport start;
    start.kind = Kinefin::PlaybackReport::Kind::Start;
    fill(start, movie, playSessionId, qint64(500'000'000));
    QCOMPARE(postReport(client, start), 204);
    Kinefin::PlaybackReport progress;
    progress.kind = Kinefin::PlaybackReport::Kind::Progress;
    fill(progress, movie, playSessionId, qint64(530'000'000));
    progress.isPaused = true;
    QCOMPARE(postReport(client, progress), 204);
    Kinefin::PlaybackReport stopped;
    stopped.kind = Kinefin::PlaybackReport::Kind::Stopped;
    fill(stopped, movie, playSessionId, qint64(530'000'000));
    QCOMPARE(postReport(client, stopped), 204);
    QCOMPARE(postReport(client, stopped), 204); // the duplicate Stopped is harmless
    qInfo() << LIVE_FACT << "Start/Progress/Stopped/duplicate-Stopped all answered 204";
    client.disconnectFromServer();
}

void TestJellyfinLive::failedStopsLeaveUserDataAlone()
{
    if (!m_live)
        QSKIP("no KINEFIN_LIVE_* environment, skipping the live checks");
    Kinefin::JellyfinClient client;
    QVERIFY(authenticate(client));
    const QJsonObject me = fetchMe(client);
    const QString movie = firstItemId(client);
    QVERIFY(!movie.isEmpty());
    const QJsonObject before =
        itemUserData(client, me.value(QStringLiteral("Id")).toString(), movie);
    Kinefin::PlaybackReport failed;
    failed.kind = Kinefin::PlaybackReport::Kind::Stopped;
    failed.failed = true;
    fill(failed, movie, QStringLiteral("kinefin-live-fiddle-psid-failed"), qint64(640'000'000));
    QCOMPARE(postReport(client, failed), 204);
    const QJsonObject after =
        itemUserData(client, me.value(QStringLiteral("Id")).toString(), movie);
    QCOMPARE(after.value(QStringLiteral("Played")).toBool(false),
             before.value(QStringLiteral("Played")).toBool(false));
    qInfo() << LIVE_FACT << "UserData fields before/after a Failed stop:"
            << "played" << before.value(QStringLiteral("Played")).toBool(false) << "->"
            << after.value(QStringLiteral("Played")).toBool(false) << "| position"
            << before.value(QStringLiteral("PlaybackPositionTicks")).toInt() << "->"
            << after.value(QStringLiteral("PlaybackPositionTicks")).toInt();
    client.disconnectFromServer();
}

void TestJellyfinLive::userDataRefreshReadsWhatAnotherSessionWrote()
{
    if (!m_live)
        QSKIP("no KINEFIN_LIVE_* environment, skipping the live checks");
    Kinefin::JellyfinClient reader;
    Kinefin::JellyfinClient writer;
    QVERIFY(authenticate(reader));
    QVERIFY(authenticate(writer));
    const QString userId = fetchMe(reader).value(QStringLiteral("Id")).toString();
    const QJsonArray rows =
        getJson(reader,
                QStringLiteral("/Items?userId=") + userId
                    + QStringLiteral(
                        "&recursive=true&includeItemTypes=Episode&filters=IsUnplayed&limit=1"))
            .value(QStringLiteral("Items"))
            .toArray();
    if (rows.isEmpty())
        QSKIP("this user has no unplayed episode");
    const QString episode = rows.first().toObject().value(QStringLiteral("Id")).toString();
    const QString series = rows.first().toObject().value(QStringLiteral("SeriesId")).toString();
    QVERIFY(!episode.isEmpty());
    QVERIFY(!series.isEmpty());
    QSignalSpy played(&reader, &Kinefin::JellyfinClient::itemPlayedUpdated);
    QSignalSpy written(&writer, &Kinefin::JellyfinClient::itemPlayedUpdated);
    const auto undo = qScopeGuard([&] {
        writer.setItemPlayed(episode, false);
        QTest::qWait(1500);
    });
    QVERIFY(writer.setItemPlayed(episode, true));
    QTRY_COMPARE_WITH_TIMEOUT(written.size(), 1, 10000);
    QVERIFY(reader.refreshUserData({episode}));
    QTRY_COMPARE_WITH_TIMEOUT(played.size(), 2, 10000);
    QCOMPARE(played.at(0).at(0).toString().toLower(), episode.toLower());
    QVERIFY(played.at(0).at(1).toBool());
    QCOMPARE(played.at(1).at(0).toString().toLower(), series.toLower());
    qInfo() << LIVE_FACT << "refreshUserData of one episode read the episode and its series:"
            << "played" << played.at(0).at(1).toBool() << "and series played"
            << played.at(1).at(1).toBool();
    reader.disconnectFromServer();
    writer.disconnectFromServer();
}

void TestJellyfinLive::hlsTranscodeLifecycle()
{
    if (!m_live)
        QSKIP("no KINEFIN_LIVE_* environment, skipping the live checks");
    Kinefin::JellyfinClient client;
    QVERIFY(authenticate(client));

    // 1. Find a movie the default profile must transcode (a 4K HDR/DoVi source)
    const QJsonObject items = getJson(
        client, QStringLiteral(
                    "/Items?IncludeItemTypes=Movie&Recursive=true&Limit=20&Fields=MediaSources"));
    const QJsonArray list = items.value(QStringLiteral("Items")).toArray();
    QString hdrItem;
    for (const auto &itemValue : list) {
        const QJsonObject item = itemValue.toObject();
        if (!hdrItem.isEmpty())
            break;
        for (const auto &sourceValue : item.value(QStringLiteral("MediaSources")).toArray()) {
            for (const auto &streamValue :
                 sourceValue.toObject().value(QStringLiteral("MediaStreams")).toArray()) {
                const QJsonObject stream = streamValue.toObject();
                if (stream.value(QStringLiteral("Type")).toString() == QStringLiteral("Video")
                    && stream.value(QStringLiteral("VideoRange")).toString()
                           != QStringLiteral("SDR")) {
                    hdrItem = item.value(QStringLiteral("Id")).toString();
                    qInfo() << LIVE_FACT << "the HDR transcode candidate item id" << hdrItem;
                    break;
                }
            }
            if (!hdrItem.isEmpty())
                break;
        }
    }
    QVERIFY2(!hdrItem.isEmpty(), "no 4K HDR/DoVi movie for the HLS cycle");

    // PlaybackInfo through the real client, with `force transcode HDR` set: a stock profile direct-plays HDR, the setting is the documented forcing
    Kinefin::DeviceProfileOptions options;
    options.forceTranscodeHdr = true;
    QSignalSpy ready(&client, &Kinefin::JellyfinClient::playbackInfoReady);
    QVERIFY(client.requestPlaybackInfo(hdrItem, qint64(0), Kinefin::buildDeviceProfile(options)));
    QVERIFY2(ready.wait(15000), "no PlaybackInfo answer within 15 s");
    const Kinefin::PlaybackSource source = ready.first().first().value<Kinefin::PlaybackSource>();
    QCOMPARE(source.itemId, hdrItem);
    QCOMPARE(source.method, Kinefin::PlaybackSource::Method::Transcode);
    // The token stays out of the URL: verify the ApiKey count in the live query
    int apiKeys = 0;
    const QList<QPair<QString, QString>> query = QUrlQuery(source.url).queryItems();
    for (const auto &pair : query)
        if (pair.first == QStringLiteral("ApiKey") || pair.first == QStringLiteral("api_key"))
            ++apiKeys;
    QCOMPARE(apiKeys, 0);
    qInfo() << LIVE_FACT << "the negotiated media URL path:" << source.url.path() << "scheme"
            << source.url.scheme() << "transcode, token-free query confirmed";

    // 3. Report the player side while the server's ffmpeg owns the HLS stream
    Kinefin::PlaybackReport start;
    start.kind = Kinefin::PlaybackReport::Kind::Start;
    fill(start, source.itemId, source.playSessionId, qint64(0));
    start.method = Kinefin::PlaybackSource::Method::Transcode;
    QCOMPARE(postReport(client, start), 204);
    QTest::qWait(1000);
    const QString during = sessionSnapshot(client, sourceDeviceId(), source.playSessionId);
    qInfo() << LIVE_FACT << "the live session record during the HLS cycle:" << during;
    QVERIFY2(!during.startsWith(QLatin1Char('(')), "the session record did not carry the play");
    Kinefin::PlaybackReport progress;
    progress.kind = Kinefin::PlaybackReport::Kind::Progress;
    fill(progress, source.itemId, source.playSessionId, qint64(3'000'000));
    progress.method = Kinefin::PlaybackSource::Method::Transcode;
    QCOMPARE(postReport(client, progress), 204);
    QTest::qWait(2000);

    // 4. The Stopped ends the server-side job: the play leaves the session record
    Kinefin::PlaybackReport stopped;
    stopped.kind = Kinefin::PlaybackReport::Kind::Stopped;
    fill(stopped, source.itemId, source.playSessionId, qint64(3'000'000));
    stopped.method = Kinefin::PlaybackSource::Method::Transcode;
    QCOMPARE(postReport(client, stopped), 204);
    QTest::qWait(4000);
    const QString after = sessionSnapshot(client, sourceDeviceId(), source.playSessionId);
    qInfo() << LIVE_FACT << "the session record after the Stopped:" << after;
    QVERIFY2(after.contains(QStringLiteral("\"nowPlayingType\":\"\"")),
             "the play survived the Stopped in the session record");
    client.disconnectFromServer();
}

void TestJellyfinLive::rawScanAndUserDataShapes()
{
    if (!m_live)
        QSKIP("no KINEFIN_LIVE_* environment, skipping the live checks");
    Kinefin::JellyfinClient client;
    QVERIFY(authenticate(client));
    QWebSocket *socket = openSocket(client);
    QVERIFY2(socket, "the capture handshake did not reach 101");
    QVERIFY(socketFirstExchange(socket));

    // A full-library scan kicks LibraryChanged frames; find the task and run it
    const QJsonArray tasks = getDocument(client, QStringLiteral("/ScheduledTasks")).array();
    if (tasks.isEmpty()) {
        // The scan-task listing is admin-only here (live 2026-11-10: GET /ScheduledTasks answers 403 for the non-admin fiddle user): the environment must hold the scan trigger for this user as in the 2026-11-08 round, where this evidence stands
        QSKIP("the environment does not grant the scan task trigger on this user");
    }
    QString scanTaskId;
    for (const auto &value : tasks) {
        const QJsonObject task = value.toObject();
        const QString name = task.value(QStringLiteral("Name")).toString();
        // 12.4: "Scan Media Library" (task key RefreshLibrary); name keys stay readable
        if (name.contains(QStringLiteral("Scan Media Library"))) {
            scanTaskId = task.value(QStringLiteral("Id")).toString();
            qInfo() << LIVE_FACT << "the scan task:" << name;
            break;
        }
    }
    QVERIFY2(!scanTaskId.isEmpty(), "the library scan task was not found");

    // The scan trigger refreshes every library; 12.4's running route is `POST /ScheduledTasks/Running/{taskId}` (no `/Begin` suffix: that answers 404)
    QCOMPARE(postJson(client, QStringLiteral("/ScheduledTasks/Running/") + scanTaskId, {}), 204);
    QTest::qWait(40000);
    // The peacetime-largest LibraryChanged measurement finally lands here: the peak byte size the server emits during a full scan decides the WS-frame table's headroom
    qsizetype peak = 0;
    QString peakRaw;
    qsizetype libraryFrames = 0;
    for (const QString &frame : m_frames) {
        if (!frame.contains(QStringLiteral("LibraryChanged")))
            continue;
        ++libraryFrames;
        if (frame.size() > peak) {
            peak = frame.size();
            peakRaw = frame;
        }
    }
    qInfo() << LIVE_FACT << "scan frames:" << libraryFrames << "LibraryChanged frames, peak" << peak
            << "characters";
    if (!peakRaw.isEmpty())
        qInfo() << LIVE_FACT << "the peak LibraryChanged frame (raw):" << peakRaw;
    for (const QString &frame : m_frames)
        qInfo() << LIVE_FACT << "in-scan frame:" << frame;

    // A user-data change needs a marker + its undo: the shapes must both pass by the socket
    const QString userId = client.userId();
    const QString movie = firstItemId(client);
    // Marking plays back HTTP 200 (not 204) in 12.4: adjust the assertion, not the route
    QCOMPARE(postJson(client,
                      QStringLiteral("/UserPlayedItems/") + movie + QStringLiteral("?userId=")
                          + userId + QStringLiteral("&datePlayed=2026-11-08T21:00:00Z"),
                      {}),
             200);
    QTest::qWait(6000);
    // The unmark answers 200 as well in 12.4 (mark/unmark behave symmetrically)
    QCOMPARE(deleteRequest(client, QStringLiteral("/UserPlayedItems/") + movie
                                       + QStringLiteral("?userId=") + userId),
             200);
    QTest::qWait(6000);
    for (const QString &frame : m_frames)
        qInfo() << LIVE_FACT << "post-marking frame:" << frame;
    socket->close(QWebSocketProtocol::CloseCodeNormal);
    client.disconnectFromServer();
}

void TestJellyfinLive::tokenLogoutClosesEverything()
{
    if (!m_live)
        QSKIP("no KINEFIN_LIVE_* environment, skipping the live checks");
    Kinefin::JellyfinClient client;
    Kinefin::JellyfinSocketClient socket(&client);
    QVERIFY(authenticate(client));
    QTRY_COMPARE(socket.state(), Kinefin::JellyfinSocketClient::State::Authenticated);

    // The server kills the token; the component follows when the credential clears
    QCOMPARE(postJson(client, QStringLiteral("/Sessions/Logout"), {}), 204);
    QTRY_COMPARE(socket.state(), Kinefin::JellyfinSocketClient::State::Disconnected);
    QCOMPARE(socket.state(), Kinefin::JellyfinSocketClient::State::Disconnected);
    QVERIFY(!client.isAuthenticated());

    // A fresh handshake with the dead token must be refused (403 upstream → no 101 events)
    QUrl url = client.serverUrl();
    url.setPath(url.path() + QStringLiteral("/socket"));
    url.setScheme(QStringLiteral("ws"));
    QNetworkRequest refused(url);
    refused.setRawHeader(
        QByteArrayLiteral("Authorization"),
        QByteArrayLiteral("MediaBrowser Client=\"Kinefin\", Device=\"dead token probe\", "
                          "DeviceId=\"kinefin-live-fiddle-dead\", Version=\"0.0.1\""));
    QWebSocket probe(QStringLiteral("Kinefin live refuse"), QWebSocketProtocol::VersionLatest,
                     this);
    QSignalSpy connected(&probe, &QWebSocket::connected);
    QSignalSpy probeError(&probe, &QWebSocket::errorOccurred);
    probe.open(refused);
    QTest::qWait(4000);
    QVERIFY2(connected.count() == 0,
             "the dead token's handshake reached 101, which must never happen");
    if (probeError.count() >= 1)
        qInfo() << LIVE_FACT << "the dead token's handshake ended in the error enum"
                << int(probeError.first().first().value<QAbstractSocket::SocketError>());
    // /Users/Me is a bare 401 after the logout; the redaction guard requires no token echo
    QCOMPARE(getDocument(client, QStringLiteral("/Users/Me"))
                 .object()
                 .value(QStringLiteral("Id"))
                 .toString(),
             QString());
    client.disconnectFromServer();
}

/* ----- engine live gate rounds; every slot runs a real mpv ----- */

// The first movie among the library's 20 whose PlaybackInfo negotiation under the default profile answers a *direct* source. PlaybackInfo runs one request at a time; a failed request just books a live fact and the loop moves on
bool TestJellyfinLive::findDirectPlayable(Kinefin::JellyfinClient &client, QString *itemId,
                                          Kinefin::PlaybackSource *source, qint64 minBitrate)
{
    const QJsonObject items = getJson(
        client, QStringLiteral(
                    "/Items?IncludeItemTypes=Movie&Recursive=true&Limit=20&Fields=MediaSources"));
    const QJsonArray list = items.value(QStringLiteral("Items")).toArray();
    if (list.isEmpty())
        return false;
    for (const auto &itemValue : list) {
        const QString candidate = itemValue.toObject().value(QStringLiteral("Id")).toString();
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::playbackInfoReady);
        QSignalSpy failed(&client, &Kinefin::JellyfinClient::playbackInfoFailed);
        if (!client.requestPlaybackInfo(candidate, qint64(0), Kinefin::buildDeviceProfile({}))) {
            qInfo() << LIVE_FACT << "PlaybackInfo was refused client-side for" << candidate;
            continue;
        }
        if (!ready.wait(20000)) {
            qInfo() << LIVE_FACT << "no PlaybackInfo answer for" << candidate << "in 20 s (failed"
                    << failed.count() << "emissions)";
            continue;
        }
        const Kinefin::PlaybackSource found =
            ready.first().first().value<Kinefin::PlaybackSource>();
        if (found.method == Kinefin::PlaybackSource::Method::Direct) {
            if (found.media.source.bitrate < minBitrate) {
                qInfo() << LIVE_FACT << "direct-playable candidate" << candidate << "is only"
                        << found.media.source.bitrate << "bit/s, trying the next";
                continue;
            }
            *itemId = candidate;
            *source = found;
            qInfo() << LIVE_FACT << "the first direct-playable candidate is" << candidate;
            return true;
        }
        qInfo() << LIVE_FACT
                << "candidate transcodes under the default profile, trying the next:" << candidate;
    }
    return false;
}

// The first movie among the library's 20 with a video stream that is not SDR (the HDR/DoVi transcode candidate; the same probe the reporting round used live)
QString TestJellyfinLive::hdrItemId(Kinefin::JellyfinClient &client)
{
    const QJsonObject items = getJson(
        client, QStringLiteral(
                    "/Items?IncludeItemTypes=Movie&Recursive=true&Limit=20&Fields=MediaSources"));
    for (const auto &itemValue : items.value(QStringLiteral("Items")).toArray()) {
        const QJsonObject item = itemValue.toObject();
        for (const auto &sourceValue : item.value(QStringLiteral("MediaSources")).toArray()) {
            for (const auto &streamValue :
                 sourceValue.toObject().value(QStringLiteral("MediaStreams")).toArray()) {
                const QJsonObject stream = streamValue.toObject();
                if (stream.value(QStringLiteral("Type")).toString() == QStringLiteral("Video")
                    && stream.value(QStringLiteral("VideoRange")).toString()
                           != QStringLiteral("SDR"))
                    return item.value(QStringLiteral("Id")).toString();
            }
        }
    }
    return {};
}

// The token must never ride the media URL
void TestJellyfinLive::assertTokenFreeMediaUrl(const QUrl &url)
{
    for (const auto &pair : QUrlQuery(url).queryItems())
        QVERIFY2(pair.first != QStringLiteral("ApiKey") && pair.first != QStringLiteral("api_key"),
                 "the media URL carries an ApiKey parameter");
}

// After the engine reached Playing: the server session record must carry the playing item
void TestJellyfinLive::recordAndAssertPlaying(Kinefin::JellyfinClient &client,
                                              const QString &deviceId,
                                              const Kinefin::PlaybackSource &source,
                                              Kinefin::PlaybackSession &session,
                                              Kinefin::PlaybackEngine *engine)
{
    QCOMPARE(session.state(), Kinefin::PlaybackSession::State::Playing);
    QCOMPARE(session.route(), source.method == Kinefin::PlaybackSource::Method::Transcode
                                  ? Kinefin::PlaybackSession::Route::Transcoded
                                  : Kinefin::PlaybackSession::Route::Direct);
    qInfo() << LIVE_FACT << "route"
            << (source.method == Kinefin::PlaybackSource::Method::Transcode ? "transcode"
                                                                            : "direct")
            << "positionTicks" << engine->positionTicks() << "durationTicks"
            << engine->durationTicks() << "canSeek" << engine->canSeek();
    // The engine's own probe values must be usable before the record check: mpv answered
    QTRY_VERIFY_WITH_TIMEOUT(engine->positionTicks() > 0, 20000);
    QTRY_VERIFY_WITH_TIMEOUT(engine->durationTicks() > 0, 20000);

    // The Start report must have landed and woven into the server session record; the snapshot is a QTRY so the cadence race cannot decide this check
    QTRY_VERIFY_WITH_TIMEOUT(sessionSnapshot(client, deviceId, source.playSessionId)
                                 .contains(QStringLiteral("\"nowPlayingItem\":\"") + source.itemId
                                           + QStringLiteral("\"")),
                             15000);
    const QString during = sessionSnapshot(client, deviceId, source.playSessionId);
    qInfo() << LIVE_FACT << "server session record during play:" << during;
}

void TestJellyfinLive::engineDirectPlayLifecycle()
{
    if (!m_live)
        QSKIP("no KINEFIN_LIVE_* environment, skipping the live checks");
    Kinefin::JellyfinClient client;
    QVERIFY(authenticate(client));
    const QString deviceId = m_deviceId;

    QString itemId;
    Kinefin::PlaybackSource source;
    QVERIFY2(findDirectPlayable(client, &itemId, &source),
             "no direct-playable movie in the first 20");
    QCOMPARE(source.itemId, itemId);
    assertTokenFreeMediaUrl(source.url);
    qInfo() << LIVE_FACT << "direct-play round: chosen media source" << source.mediaSourceId
            << "play session" << source.playSessionId << "media path" << source.url.path();

    // Canonical chain: the engine, then the reporter on its session. The container is offscreen; the null outputs are the proven fixture route
    Kinefin::PlaybackSession session;
    auto *engine = new Kinefin::PlaybackEngine(client, session, this);
    QVERIFY(engine->init({QStringLiteral("vo=null"), QStringLiteral("ao=null")}));
    Kinefin::PlaybackReporter reporter(session, client, [&engine] {
        return Kinefin::PlaybackReporter::PlayerState{.positionTicks = engine->knownPositionTicks(),
                                                      .canSeek = engine->canSeek()};
    });
    QSignalSpy ended(engine, &Kinefin::PlaybackEngine::streamEnded);
    QVERIFY(engine->open(source));
    QTRY_VERIFY_WITH_TIMEOUT(session.state() == Kinefin::PlaybackSession::State::Playing, 30000);
    recordAndAssertPlaying(client, deviceId, source, session, engine);

    // Pause holds: the position freezes for real and one in-flight tick is the whole slack
    engine->pause();
    QCOMPARE(session.state(), Kinefin::PlaybackSession::State::Paused);
    const qint64 atPause = engine->positionTicks();
    QTest::qWait(1500);
    QVERIFY2(engine->positionTicks() <= atPause + 2 * 10000000LL,
             "a paused mpv kept advancing beyond the in-flight tick");
    engine->resume();
    QTRY_VERIFY_WITH_TIMEOUT(session.state() == Kinefin::PlaybackSession::State::Playing, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(engine->positionTicks() > atPause, 10000);

    // Absolute seek, once the runtime carries it (the null/null pacing runs fast)
    const qint64 duration = engine->durationTicks();
    if (duration > 20 * 60 * 10000000LL) {
        engine->seekTo(duration / 2);
        QTest::qWait(1200);
        const qint64 afterSeek = engine->positionTicks();
        QVERIFY2(afterSeek >= duration / 3, "the absolute seek did not move the position");
        qInfo() << LIVE_FACT << "after the absolute seek to the mid-point, positionTicks"
                << afterSeek << "of" << duration;
    } else {
        qInfo() << LIVE_FACT << "the direct candidate is under 20 min; the seek is skipped, ticks"
                << engine->positionTicks();
    }

    // Stop: the reporter's Stopped goes out with the teardown; then the record check
    engine->stop();
    QCOMPARE(session.state(), Kinefin::PlaybackSession::State::Stopping);
    QTRY_VERIFY_WITH_TIMEOUT(session.state() == Kinefin::PlaybackSession::State::Idle, 30000);
    QCOMPARE(ended.size(), 1);
    QCOMPARE(ended.first().first().toString(), QStringLiteral("stop"));

    // The Stopped report carries the teardown into the server record (the r2 semantics, now produced by the real reporter path)
    QTRY_VERIFY_WITH_TIMEOUT(sessionSnapshot(client, deviceId, source.playSessionId)
                                 .contains(QStringLiteral("\"nowPlayingType\":\"\"")),
                             15000);
    const QString afterStop = sessionSnapshot(client, deviceId, source.playSessionId);
    qInfo() << LIVE_FACT << "server session record after stop:" << afterStop;
    engine->deleteLater();
    client.disconnectFromServer();
}

void TestJellyfinLive::engineTranscodeLifecycle()
{
    if (!m_live)
        QSKIP("no KINEFIN_LIVE_* environment, skipping the live checks");
    Kinefin::JellyfinClient client;
    QVERIFY(authenticate(client));
    const QString deviceId = m_deviceId;

    const QString hdrId = hdrItemId(client);
    QVERIFY2(!hdrId.isEmpty(), "no HDR/DoVi movie for the transcode round");
    Kinefin::DeviceProfileOptions options;
    options.forceTranscodeHdr = true;
    QSignalSpy ready(&client, &Kinefin::JellyfinClient::playbackInfoReady);
    QVERIFY(client.requestPlaybackInfo(hdrId, qint64(0), Kinefin::buildDeviceProfile(options)));
    QVERIFY2(ready.wait(20000), "no PlaybackInfo answer for the HDR negotiation");
    const Kinefin::PlaybackSource source = ready.first().first().value<Kinefin::PlaybackSource>();
    QCOMPARE(source.itemId, hdrId);
    QCOMPARE(source.method, Kinefin::PlaybackSource::Method::Transcode);
    assertTokenFreeMediaUrl(source.url);
    qInfo() << LIVE_FACT << "transcode round: negotiated route:" << source.url.scheme()
            << source.url.path();

    Kinefin::PlaybackSession session;
    auto *engine = new Kinefin::PlaybackEngine(client, session, this);
    QVERIFY(engine->init({QStringLiteral("vo=null"), QStringLiteral("ao=null")}));
    Kinefin::PlaybackReporter reporter(session, client, [&engine] {
        return Kinefin::PlaybackReporter::PlayerState{.positionTicks = engine->knownPositionTicks(),
                                                      .canSeek = engine->canSeek()};
    });
    QSignalSpy ended(engine, &Kinefin::PlaybackEngine::streamEnded);
    QVERIFY(engine->open(source));
    QTRY_VERIFY_WITH_TIMEOUT(session.state() == Kinefin::PlaybackSession::State::Playing, 40000);
    recordAndAssertPlaying(client, deviceId, source, session, engine);
    qInfo() << LIVE_FACT << "the live HLS route played; letting the server encode for a moment";
    QTest::qWait(4000);

    engine->stop();
    QTRY_VERIFY_WITH_TIMEOUT(session.state() == Kinefin::PlaybackSession::State::Idle, 40000);
    QCOMPARE(ended.size(), 1);
    QCOMPARE(ended.first().first().toString(), QStringLiteral("stop"));
    // The Stopped tears the server job down: the session record loses the play (the ffmpeg process death itself is verified server-side in the round notes, not by this probe)
    QTest::qWait(4000);
    const QString afterStop = sessionSnapshot(client, deviceId, source.playSessionId);
    qInfo() << LIVE_FACT << "server session record after stop:" << afterStop;
    QVERIFY2(afterStop.contains(QStringLiteral("\"nowPlayingType\":\"\"")),
             "the transcode survived the stop in the server session record");
    engine->deleteLater();
    client.disconnectFromServer();
}

void TestJellyfinLive::transcodeCacheDump()
{
    if (!m_live)
        QSKIP("no KINEFIN_LIVE_* environment, skipping the live checks");
    Kinefin::JellyfinClient client;
    QVERIFY(authenticate(client));
    QString itemId;
    Kinefin::PlaybackSource direct;
    QVERIFY2(findDirectPlayable(client, &itemId, &direct, 3'000'000),
             "no direct-playable movie of at least 3 Mbit/s");
    // A cap under the file's rate: the server transcodes
    QSignalSpy ready(&client, &Kinefin::JellyfinClient::playbackInfoReady);
    QVERIFY(
        client.requestPlaybackInfo(itemId, qint64(0), Kinefin::buildDeviceProfile({}), 2'000'000));
    QVERIFY2(ready.wait(20000), "no PlaybackInfo answer for the capped negotiation");
    const Kinefin::PlaybackSource source = ready.first().first().value<Kinefin::PlaybackSource>();
    QCOMPARE(source.method, Kinefin::PlaybackSource::Method::Transcode);

    Kinefin::PlaybackSession session;
    auto *engine = new Kinefin::PlaybackEngine(client, session, this);
    QVERIFY(engine->init({QStringLiteral("vo=null"), QStringLiteral("ao=null")}));
    Kinefin::PlaybackReporter reporter(session, client, [&engine] {
        return Kinefin::PlaybackReporter::PlayerState{.positionTicks = engine->knownPositionTicks(),
                                                      .canSeek = engine->canSeek()};
    });
    QVERIFY(engine->open(source));
    QTRY_VERIFY_WITH_TIMEOUT(session.state() == Kinefin::PlaybackSession::State::Playing, 40000);
    QTRY_VERIFY_WITH_TIMEOUT(engine->mpvProperty(QStringLiteral("time-pos")).toDouble() >= 25.0,
                             60000);

    const double position = engine->mpvProperty(QStringLiteral("time-pos")).toDouble();
    const double a = position - 20.0;
    const double b = position - 2.0;
    // A failed check leaves nothing behind in the temporary folder
    QTemporaryDir work;
    QVERIFY(work.isValid());
    const QString dump = work.filePath(QStringLiteral("dump.ts"));
    QElapsedTimer blocked;
    blocked.start();
    engine->setMpvProperty(QStringLiteral("ab-loop-a"), a);
    engine->setMpvProperty(QStringLiteral("ab-loop-b"), b);
    const bool aligned = engine->mpvCommand({QStringLiteral("ab-loop-align-cache")});
    const double alignedA = engine->mpvProperty(QStringLiteral("ab-loop-a")).toDouble();
    const double alignedB = engine->mpvProperty(QStringLiteral("ab-loop-b")).toDouble();
    const bool dumped = engine->mpvCommand({QStringLiteral("ab-loop-dump-cache"), dump});
    engine->setMpvProperty(QStringLiteral("ab-loop-a"), QStringLiteral("no"));
    engine->setMpvProperty(QStringLiteral("ab-loop-b"), QStringLiteral("no"));
    const qint64 blockedMs = blocked.elapsed();
    const double after = engine->mpvProperty(QStringLiteral("time-pos")).toDouble();
    qInfo() << LIVE_FACT << "transcode dump marks" << a << b << "aligned" << aligned << "start"
            << alignedA << "dumped" << dumped << "bytes" << QFileInfo(dump).size() << "blocked ms"
            << blockedMs << "position before" << position << "after" << after;
    qInfo() << LIVE_FACT << "transcode dump aligned end" << alignedB << "dump duration"
            << MpvProbe::duration(dump);
    QVERIFY2(aligned && dumped, "mpv refused ab-loop-align-cache or ab-loop-dump-cache");
    QVERIFY(QFileInfo(dump).size() > 0);
    QVERIFY2(alignedA <= a + 0.001 && a - alignedA < 12.0,
             "the aligned start is after the mark or more than a GOP before it");
    // The player went on: no loop back to A
    QTest::qWait(1500);
    QVERIFY(engine->mpvProperty(QStringLiteral("time-pos")).toDouble() > after);

    // The dump encodes from the mark on, as a clip of a local file does
    Kinefin::ClipExporter exporter;
    QSignalSpy finished(&exporter, &Kinefin::ClipExporter::finished);
    QSignalSpy failed(&exporter, &Kinefin::ClipExporter::failed);
    Kinefin::ClipRequest request;
    request.source = QUrl::fromLocalFile(dump);
    request.startTicks = qint64((a - alignedA) * 10'000'000);
    request.endTicks = qint64((b - alignedA) * 10'000'000);
    request.decodeFromFileStart = true;
    const QString staging = work.filePath(QStringLiteral("clip.mp4"));
    QVERIFY(exporter.start(request, staging));
    QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty() || !failed.isEmpty(), 120000);
    const double clipDuration = MpvProbe::duration(staging);
    qInfo() << LIVE_FACT << "transcode dump clip duration" << clipDuration << "expected" << b - a;
    qInfo() << LIVE_FACT << "transcode dump encoded clip bytes" << QFileInfo(staging).size()
            << "failed" << failed.size();
    QCOMPARE(failed.size(), 0);
    QVERIFY(QFileInfo(staging).size() > 0);
    QVERIFY2(qAbs(clipDuration - (b - a)) < 0.5, qPrintable(QString::number(clipDuration)));

    engine->stop();
    QTRY_VERIFY_WITH_TIMEOUT(session.state() == Kinefin::PlaybackSession::State::Idle, 40000);
    engine->deleteLater();
    client.disconnectFromServer();
}

void TestJellyfinLive::transcodeClipThroughThePlayer()
{
    if (!m_live)
        QSKIP("no KINEFIN_LIVE_* environment, skipping the live checks");
    Kinefin::JellyfinClient client;
    QVERIFY(authenticate(client));
    QString itemId;
    Kinefin::PlaybackSource direct;
    QVERIFY2(findDirectPlayable(client, &itemId, &direct, 3'000'000),
             "no direct-playable movie of at least 3 Mbit/s");
    // The profile's files (the clips folder among them) go to a folder of the test's own
    QTemporaryDir data;
    QVERIFY(data.isValid());
    const QByteArray previousData = qgetenv("XDG_DATA_HOME");
    qputenv("XDG_DATA_HOME", data.path().toUtf8());
    const auto restoreData = qScopeGuard([&previousData] {
        if (previousData.isEmpty())
            qunsetenv("XDG_DATA_HOME");
        else
            qputenv("XDG_DATA_HOME", previousData);
    });
    Kinefin::PlayQueue queue(&client);
    Kinefin::ClientSettings settings(data.filePath(QStringLiteral("kinefin.conf")));
    Kinefin::PlayerController controller(client, queue, settings, QStringLiteral("liveclip"));
    auto *engine = controller.findChild<Kinefin::PlaybackEngine *>();
    QVERIFY(engine);
    QVERIFY(engine->init({QStringLiteral("vo=null"), QStringLiteral("ao=null")}));
    QSignalSpy infos(&client, &Kinefin::JellyfinClient::playbackInfoReady);
    const auto playing = [&controller] {
        return controller.active() && !controller.loading() && !controller.buffering()
               && !controller.paused() && controller.canSeek();
    };
    constexpr qint64 second = 10'000'000;

    QVERIFY(controller.playItem(itemId, 0));
    QTRY_VERIFY_WITH_TIMEOUT(playing(), 30000);
    // A step under the file's rate: the server transcodes from the position on
    controller.setQualityBitrate(2'000'000);
    QTRY_VERIFY_WITH_TIMEOUT(infos.size() >= 2
                                 && infos.last().first().value<Kinefin::PlaybackSource>().method
                                        == Kinefin::PlaybackSource::Method::Transcode
                                 && playing(),
                             60000);
    const qint64 from = controller.positionTicks();
    QTRY_VERIFY_WITH_TIMEOUT(controller.positionTicks() > from + 4 * second, 30000);
    controller.markClipStart();
    QTRY_VERIFY_WITH_TIMEOUT(controller.positionTicks() > controller.clipStartTicks() + 6 * second,
                             30000);
    controller.markClipEnd();
    QTRY_VERIFY2_WITH_TIMEOUT(controller.clipVideoEnabled(), qPrintable(controller.clipReason()),
                              10000);

    QSignalSpy saved(&controller, &Kinefin::PlayerController::clipSaved);
    QSignalSpy failed(&controller, &Kinefin::PlayerController::clipSaveFailed);
    const QString target = data.filePath(
        Kinefin::ClipExporter::hasEncoder(QStringLiteral("libx264")) ? QStringLiteral("clip.mp4")
                                                                     : QStringLiteral("clip.webm"));
    QElapsedTimer blocked;
    blocked.start();
    QVERIFY(controller.saveClip(false, QUrl::fromLocalFile(target)));
    qInfo() << LIVE_FACT << "transcode clip: saveClip blocked ms" << blocked.elapsed() << "marks"
            << controller.clipStartTicks() << controller.clipEndTicks();
    QTRY_VERIFY_WITH_TIMEOUT(!saved.isEmpty() || !failed.isEmpty(), 120000);
    QCOMPARE(failed.size(), 0);
    QCOMPARE(saved.size(), 1);
    const double duration = MpvProbe::duration(target);
    qInfo() << LIVE_FACT << "transcode clip bytes" << QFileInfo(target).size() << "duration"
            << duration;
    QVERIFY2(duration > 5.0 && duration < 7.5, qPrintable(QString::number(duration)));
    const QDir clips(data.filePath(QStringLiteral("kinefin/profiles/liveclip/clips")));
    QCOMPARE(clips.entryList(QDir::Files), QStringList());

    controller.stop();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.active(), 30000);
    client.disconnectFromServer();
}

void TestJellyfinLive::engineProgressAdvancesUserData()
{
    if (!m_live)
        QSKIP("no KINEFIN_LIVE_* environment, skipping the live checks");
    Kinefin::JellyfinClient client;
    QVERIFY(authenticate(client));
    const QString userId = client.userId();

    QString itemId;
    Kinefin::PlaybackSource source;
    QVERIFY2(findDirectPlayable(client, &itemId, &source),
             "no direct-playable movie in the first 20");
    const QJsonObject before = itemUserData(client, userId, itemId);
    qInfo() << LIVE_FACT << "progress round: user data before the round: playbackPositionTicks"
            << before.value(QStringLiteral("PlaybackPositionTicks")).toInt() << "played"
            << before.value(QStringLiteral("Played")).toBool(false);

    Kinefin::PlaybackSession session;
    auto *engine = new Kinefin::PlaybackEngine(client, session, this);
    QVERIFY(engine->init({QStringLiteral("vo=null"), QStringLiteral("ao=null")}));
    Kinefin::PlaybackReporter reporter(session, client, [&engine] {
        return Kinefin::PlaybackReporter::PlayerState{.positionTicks = engine->knownPositionTicks(),
                                                      .canSeek = engine->canSeek()};
    });
    QVERIFY(engine->open(source));
    QTRY_VERIFY_WITH_TIMEOUT(session.state() == Kinefin::PlaybackSession::State::Playing, 30000);
    recordAndAssertPlaying(client, m_deviceId, source, session, engine);

    // Let the (fast null/null) traverse advance past the sub-second stop the server would refuse to treat as progress at all (~2 s of wall, seconds of movie time)
    QTest::qWait(2000);

    // >20 s of *realtime*: mpv is held paused, and the reporter keeps the cadence alive the whole time (the server must not judge the playback gone)
    engine->pause();
    QCOMPARE(session.state(), Kinefin::PlaybackSession::State::Paused);
    const qint64 pausedAt = engine->positionTicks();
    QTest::qWait(12000);
    const QString recordA = sessionSnapshot(client, m_deviceId, source.playSessionId);
    QTest::qWait(12000);
    const QString recordB = sessionSnapshot(client, m_deviceId, source.playSessionId);
    QVERIFY2(!recordA.startsWith(QLatin1Char('(')) && !recordB.startsWith(QLatin1Char('(')),
             "the session record vanished during the held pause");
    const qint64 ticksA = QJsonDocument::fromJson(recordA.toUtf8())
                              .object()
                              .value(QStringLiteral("positionTicks"))
                              .toInt(-1);
    const qint64 ticksB = QJsonDocument::fromJson(recordB.toUtf8())
                              .object()
                              .value(QStringLiteral("positionTicks"))
                              .toInt(-1);
    QVERIFY2(ticksA >= 0 && ticksB >= 0, "the session record carried no position ticks");
    qInfo() << LIVE_FACT << "server record position during the held pause:" << ticksA << "->"
            << ticksB << "(the engine paused at" << pausedAt << ")";
    // The record stays at the pause point (one report cadence may refresh the exact value)
    QVERIFY2(qAbs(ticksB - ticksA) <= 2 * 10000000LL,
             "the server position moved during a held pause");

    engine->resume();
    QTRY_VERIFY_WITH_TIMEOUT(session.state() == Kinefin::PlaybackSession::State::Playing, 10000);
    engine->stop();
    QTRY_VERIFY_WITH_TIMEOUT(session.state() == Kinefin::PlaybackSession::State::Idle, 30000);
    QTest::qWait(3000);
    const QJsonObject after = itemUserData(client, userId, itemId);
    const qint64 beforeTicks = before.value(QStringLiteral("PlaybackPositionTicks")).toInt(-1);
    const qint64 afterTicks = after.value(QStringLiteral("PlaybackPositionTicks")).toInt(-1);
    qInfo() << LIVE_FACT
            << "progress round: continue point after the engine's own stop:" << beforeTicks << "->"
            << afterTicks;
    if (afterTicks <= beforeTicks) {
        // Is the position too small to persist, or does 12.4 not write the continue point from reports at all? Decisive probe: the same Stopped body with a large position, posted by hand through the r-round helper
        Kinefin::PlaybackReport manualStop;
        manualStop.kind = Kinefin::PlaybackReport::Kind::Stopped;
        fill(manualStop, itemId, source.playSessionId, qint64(240 * 10000000LL));
        manualStop.mediaSourceId = source.mediaSourceId;
        const int status = postReport(client, manualStop);
        QTest::qWait(3000);
        const QJsonObject manualAfter = itemUserData(client, userId, itemId);
        const qint64 manualTicks =
            manualAfter.value(QStringLiteral("PlaybackPositionTicks")).toInt(-1);
        qInfo() << LIVE_FACT << "manual Stopped status" << status << "carried 240 s; continue point"
                << beforeTicks << "->" << manualTicks;
        QVERIFY2(manualTicks > 0 || manualTicks == beforeTicks,
                 "the 12.4 server rejected a large-position Stopped body the r-round accepted");
    }
    engine->deleteLater();
    client.disconnectFromServer();
}

/* The local byte forwarder behind the connection-cut round and the subpath BaseUrl round: the client and player both speak to it, it speaks to the real server. With an empty prefix the forwarded bytes leave untouched (cut() drops every connection, both directions). With a prefix it rewrites request line paths DOWN→UP, so a subpath BaseUrl (`<host>/jellyfin` deployments) behaves like the proxy-less case. setDownstreamRate paces the server-to-client bytes (a throttled link) and reports() lists the playback Start and Stopped requests that went upstream */
class FixtureProxy : public QObject {
    Q_OBJECT

  public:
    explicit FixtureProxy(QObject *parent = nullptr) : QObject(parent)
    {
        connect(&m_listener, &QTcpServer::newConnection, this, &FixtureProxy::accept);
        m_pacer.setInterval(1000 / PacerTicksPerSecond);
        connect(&m_pacer, &QTimer::timeout, this, &FixtureProxy::pace);
    }
    // The members go before the sockets the listener and this object own: a socket closing then would run `retire` on a destroyed m_downOf, so the sockets lose their slots first
    ~FixtureProxy() override { cut(); }

    bool start(const QUrl &upstream, const QByteArray &prefix = {})
    {
        m_prefix = prefix;
        m_upstreamHost = upstream.host();
        m_upstreamPort = quint16(upstream.port(8096));
        return m_listener.listen(QHostAddress::LocalHost);
    }
    quint16 port() const { return m_listener.serverPort(); }
    // Server to client bytes per second, 0 for unthrottled; takes effect at once, also on open connections
    void setDownstreamRate(qint64 bytesPerSecond)
    {
        m_rate = qMax(qint64(0), bytesPerSecond);
        for (auto it = m_downOf.cbegin(); it != m_downOf.cend(); ++it)
            it.key()->setReadBufferSize(m_rate > 0 || m_backpressure ? BoundedBuffer : 0);
        if (m_rate > 0) {
            m_pacer.start();
            return;
        }
        m_pacer.stop();
        const auto pairs = m_downOf;
        for (auto it = pairs.cbegin(); it != pairs.cend(); ++it)
            pump(it.key(), it.value());
    }
    // From now on the proxy moves no more server bytes than the client has taken (before any connection opens): without it a client that reads slowly sees a proxy that already holds everything the server could send, and a later throttle has nothing to bite on
    void boundBuffers() { m_backpressure = true; }
    // One line per upstream Start or Stopped request, in order, with the PlaySessionId when the body came with the request line
    QStringList reports() const { return m_reports; }
    // Bytes that went server to client so far
    qint64 downstreamBytes() const { return m_downBytes; }

    void cut()
    {
        m_listener.close();
        m_pacer.stop();
        m_downOf.clear();
        for (auto *socket : m_sockets) {
            // Drop this object's own slots first: a queued readyRead must not reach a socket that is already closed or deleted
            socket->disconnect();
            socket->close();
            socket->abort();
            socket->deleteLater();
        }
        m_sockets.clear();
    }

  private:
    static constexpr int PacerTicksPerSecond = 20;
    static constexpr int BoundedBuffer = 256 * 1024;

    // Moves the server's bytes to the client: all of them, or, with bounded buffers, only while the client's socket holds less than the bound; the rest stays in `up`, so the server's TCP window closes. Under a throttle the pacer moves them
    void pump(QTcpSocket *up, QTcpSocket *down)
    {
        if (m_rate > 0)
            return;
        if (!m_backpressure) {
            relay(up, down, -1);
            return;
        }
        while (up->bytesAvailable() > 0 && down->bytesToWrite() < BoundedBuffer)
            relay(up, down, BoundedBuffer);
    }

    // Moves what `up` holds to `down`, at most `limit` bytes (-1 for all)
    void relay(QTcpSocket *up, QTcpSocket *down, qint64 limit)
    {
        const QByteArray data = limit < 0 ? up->readAll() : up->read(limit);
        if (!data.isEmpty())
            down->write(data);
        m_downBytes += data.size();
    }

    void noteReport(const QByteArray &data)
    {
        const bool isStart = data.startsWith("POST /Sessions/Playing HTTP");
        if (!isStart && !data.startsWith("POST /Sessions/Playing/Stopped HTTP"))
            return;
        static const QRegularExpression sessionId(QStringLiteral("\"PlaySessionId\":\"([^\"]*)\""));
        const QRegularExpressionMatch match = sessionId.match(QString::fromUtf8(data));
        m_reports.append((isStart ? QStringLiteral("Start ") : QStringLiteral("Stopped "))
                         + (match.hasMatch() ? match.captured(1) : QStringLiteral("(body apart)")));
    }

    // Rewrites only when the chunk is the start of an HTTP request and the request line carries our prefix; chunked-first-line splits would skew the rewrite, which for the fixture's single-Qt-write requests does not occur. Upstream responses pass as-is (the server never learned about the prefix)
    static QByteArray stripPrefixFromRequest(QByteArray data, const QByteArray &prefix)
    {
        const qsizetype lineEnd = data.indexOf(QByteArrayLiteral("\r\n"));
        if (lineEnd < 0) {
            return data;
        }
        QByteArray line = data.left(lineEnd);
        const qsizetype urlStart = line.indexOf(' ');
        const qsizetype urlEnd = line.lastIndexOf(' ');
        if (urlStart < 0 || urlEnd <= urlStart) {
            return data;
        }
        QByteArray url = line.mid(urlStart + 1, urlEnd - urlStart - 1);
        if (url.startsWith(prefix)) {
            if (url.size() == prefix.size()) {
                url = QByteArrayLiteral("/");
            } else if (url.mid(prefix.size(), 1) == QByteArrayLiteral("/")) {
                url = url.mid(prefix.size()); // a leading / on the stripped shape keeps \r\n intact
            }
        }
        line = line.left(urlStart + 1) + url + line.mid(urlEnd);
        return line + data.mid(lineEnd);
    }

  private Q_SLOTS:
    void accept()
    {
        while (QTcpSocket *down = m_listener.nextPendingConnection()) {
            auto *up = new QTcpSocket(this);
            up->connectToHost(m_upstreamHost, m_upstreamPort);
            const auto forward = [this](QTcpSocket *from, QTcpSocket *to, const bool strip) {
                // Qt buffers these writes while the peer is still connecting; they flush on the connection, so no guard is needed and none must exist: dropping the first request bytes here is what broke the earlier round
                QObject::connect(from, &QAbstractSocket::readyRead, from, [this, from, to, strip] {
                    if (!strip) {
                        pump(from, to);
                        return;
                    }
                    QByteArray data = from->readAll();
                    noteReport(data);
                    if (!m_prefix.isEmpty())
                        data = stripPrefixFromRequest(data, m_prefix);
                    to->write(data);
                });
            };
            forward(down, up, true);
            forward(up, down, false);
            m_downOf.insert(up, down);
            if (m_rate > 0 || m_backpressure)
                up->setReadBufferSize(BoundedBuffer);
            if (m_backpressure)
                QObject::connect(down, &QAbstractSocket::bytesWritten, down,
                                 [this, up, down] { pump(up, down); });
            // Either side's close tears both down together: the survivor must never hold a dangling peer. removeOne compares pointers only, so a half-destroyed pair is still safe to unregister
            const auto retire = [this, down = QPointer<QTcpSocket>(down),
                                 up = QPointer<QTcpSocket>(up)] {
                // A server that answers and closes (ffmpeg asks for Connection: close) leaves its answer in `up` while a throttle holds it: the client gets that tail before the pair closes, and its socket closes only once it wrote it
                if (up && down && up->bytesAvailable() > 0
                    && down->state() == QAbstractSocket::ConnectedState)
                    relay(up, down, -1);
                m_sockets.removeOne(down.data());
                m_sockets.removeOne(up.data());
                m_downOf.remove(up.data());
                if (up) {
                    up->close();
                    up->deleteLater();
                }
                if (!down)
                    return;
                if (down->bytesToWrite() > 0 && down->state() == QAbstractSocket::ConnectedState) {
                    connect(down, &QAbstractSocket::disconnected, down, &QObject::deleteLater);
                    down->disconnectFromHost();
                } else {
                    down->close();
                    down->deleteLater();
                }
            };
            QObject::connect(down, &QAbstractSocket::disconnected, this, retire);
            QObject::connect(up, &QAbstractSocket::disconnected, this, retire);
            m_sockets.append(down);
            m_sockets.append(up);
        }
    }

    // One tick of the throttle: the budget of the tick is split among the connections that have bytes waiting
    void pace()
    {
        const qint64 budget = qMax(qint64(1), m_rate / PacerTicksPerSecond);
        const auto pairs = m_downOf;
        QList<QTcpSocket *> waiting;
        for (auto it = pairs.cbegin(); it != pairs.cend(); ++it)
            if (it.key()->bytesAvailable() > 0 && it.value()->bytesToWrite() < BoundedBuffer)
                waiting.append(it.key());
        for (QTcpSocket *up : std::as_const(waiting)) {
            // A write below can retire a pair
            if (m_downOf.contains(up))
                relay(up, pairs.value(up), qMax(qint64(1), budget / waiting.size()));
        }
    }

  private:
    QTcpServer m_listener;
    QTimer m_pacer;
    qint64 m_rate = 0;
    qint64 m_downBytes = 0;
    bool m_backpressure = false;
    QHash<QTcpSocket *, QTcpSocket *> m_downOf;
    QStringList m_reports;
    QString m_upstreamHost;
    quint16 m_upstreamPort = 0;
    QByteArray m_prefix;
    QList<QTcpSocket *> m_sockets;
};

void TestJellyfinLive::engineInterruptionSeversAndRecovers()
{
    if (!m_live)
        QSKIP("no KINEFIN_LIVE_* environment, skipping the live checks");
    Kinefin::JellyfinClient client;
    QVERIFY(authenticate(client));
    QString itemId;
    Kinefin::PlaybackSource source;
    QVERIFY2(findDirectPlayable(client, &itemId, &source),
             "no direct-playable movie in the first 20");

    FixtureProxy proxy(this);
    QVERIFY2(proxy.start(m_base), "the local forwarder could not listen");
    QUrl through = m_base;
    through.setHost(QStringLiteral("127.0.0.1"));
    through.setPort(int(proxy.port()));
    qInfo() << LIVE_FACT << "connection-cut round: forwarder on" << through.toString();

    // The playing side lives entirely behind the proxy; loopback auth needs no opt-in
    Kinefin::JellyfinClient proxyClient;
    ++deviceSequence;
    const QString proxyDeviceId = QStringLiteral("kinefin-live-fiddle-%1").arg(deviceSequence);
    QVERIFY(proxyClient.setDeviceIdentity(QStringLiteral("Kinefin"), proxyDeviceId,
                                          QStringLiteral("0.0.1")));
    QVERIFY(proxyClient.connectToServer(through));
    QSignalSpy proxyReady(&proxyClient, &Kinefin::JellyfinClient::serverReady);
    QVERIFY2(proxyReady.wait(20000), "the server probe through the forwarder never answered");
    QSignalSpy loggedIn(&proxyClient, &Kinefin::JellyfinClient::loginSucceeded);
    QVERIFY(proxyClient.authenticate(m_user, m_password));
    QVERIFY2(loggedIn.wait(20000), "the proxied login did not arrive");

    // The stream goes through the proxy too: the same scheme/host rewrite
    QSignalSpy ready(&proxyClient, &Kinefin::JellyfinClient::playbackInfoReady);
    QVERIFY(proxyClient.requestPlaybackInfo(itemId, qint64(0), Kinefin::buildDeviceProfile({})));
    QVERIFY2(ready.wait(20000), "no PlaybackInfo answer through the forwarder");
    Kinefin::PlaybackSource throughSource = ready.first().first().value<Kinefin::PlaybackSource>();
    throughSource.url.setHost(QStringLiteral("127.0.0.1"));
    throughSource.url.setPort(int(proxy.port()));

    Kinefin::PlaybackSession session;
    auto *engine = new Kinefin::PlaybackEngine(proxyClient, session, this);
    // ffmpeg's http reconnect would silently reopen the cut stream forever, and the default 150 MiB demuxer cache would outlive the test window: keep the read-ahead small so the severed stream surfaces as one end-file error quickly
    QVERIFY(engine->init({QStringLiteral("vo=null"), QStringLiteral("ao=null"),
                          QStringLiteral("stream-lavf-o=reconnect=0,reconnect_streamed=0,"
                                         "reconnect_at_eof=0,reconnect_on_network_error=0"),
                          QStringLiteral("demuxer-max-bytes=5MiB")}));
    Kinefin::PlaybackReporter reporter(session, proxyClient, [&engine] {
        return Kinefin::PlaybackReporter::PlayerState{.positionTicks = engine->knownPositionTicks(),
                                                      .canSeek = engine->canSeek()};
    });
    QSignalSpy ended(engine, &Kinefin::PlaybackEngine::streamEnded);
    QVERIFY(engine->open(throughSource));
    QTRY_VERIFY_WITH_TIMEOUT(session.state() == Kinefin::PlaybackSession::State::Playing, 30000);
    qInfo() << LIVE_FACT << "playing through the forwarder; ticks" << engine->positionTicks();
    QTest::qWait(2000);

    // The cut: the player's stream dies; the reporter's queued traffic cannot leave
    proxy.cut();
    // The demuxer cache drains at realtime pace, so the teardown lands after the cache is consumed; the reading mpv does meanwhile is from the cache alone
    QTRY_VERIFY_WITH_TIMEOUT(session.state() == Kinefin::PlaybackSession::State::Idle, 180000);
    QCOMPARE(ended.size(), 1);
    // A severed wire reads as a premature EOF at mpv's layer (the "Stream ends prematurely" error path), not as an end-file error; either reason settles the same machine
    qInfo() << LIVE_FACT << "the cut surfaced as one end-file" << ended.first().first().toString();
    qInfo() << LIVE_FACT << "after the cut the session is Idle with exactly one streamEnded";

    // One canonical playback identity and a bounded report queue: nothing second-session formed, and the queue drained itself in bounded attempts (only a Stopped retries, once)
    QTRY_COMPARE_WITH_TIMEOUT(proxyClient.hasPendingPlaybackReports(), false, 30000);
    qInfo() << LIVE_FACT << "the proxy client's report queue is drained";
    const QJsonArray sessions = getDocument(client, QStringLiteral("/Sessions")).array();
    int deviceSessions = 0;
    for (const auto &value : sessions)
        if (value.toObject().value(QStringLiteral("DeviceId")).toString() == proxyDeviceId)
            ++deviceSessions;
    QVERIFY2(deviceSessions <= 1, "more than one session record for the interrupted device");
    qInfo() << LIVE_FACT << "sessions recorded for the interrupted device:" << deviceSessions;

    // Recovery is the real base: a fresh client authenticates like an app restart
    Kinefin::JellyfinClient recover;
    QVERIFY2(authenticate(recover), "re-authentication after the interruption failed");
    recover.disconnectFromServer();

    // The stale header never rides a credential-less open: cleared, then the truth
    engine->deleteLater();
    proxyClient.disconnectFromServer();
    client.disconnectFromServer();
}

void TestJellyfinLive::playerFallsBackOnAThrottledLink()
{
    if (!m_live)
        QSKIP("no KINEFIN_LIVE_* environment, skipping the live checks");
    constexpr qint64 second = 10'000'000;
    Kinefin::JellyfinClient client;
    QVERIFY(authenticate(client));
    QString itemId;
    Kinefin::PlaybackSource source;
    QVERIFY2(findDirectPlayable(client, &itemId, &source, 5'000'000),
             "no direct-playable movie of 5 Mbit/s or more in the first 20");
    const qint64 sourceBitrate = source.media.source.bitrate;
    qInfo() << LIVE_FACT << "fallback round movie" << itemId << "source bitrate" << sourceBitrate
            << "bit/s";

    FixtureProxy proxy(this);
    proxy.boundBuffers();
    QVERIFY2(proxy.start(m_base), "the local forwarder could not listen");
    QUrl through = m_base;
    through.setHost(QStringLiteral("127.0.0.1"));
    through.setPort(int(proxy.port()));

    Kinefin::JellyfinClient proxyClient;
    ++deviceSequence;
    const QString proxyDeviceId = QStringLiteral("kinefin-live-fiddle-%1").arg(deviceSequence);
    QVERIFY(proxyClient.setDeviceIdentity(QStringLiteral("Kinefin"), proxyDeviceId,
                                          QStringLiteral("0.0.1")));
    QVERIFY(proxyClient.connectToServer(through));
    QSignalSpy proxyReady(&proxyClient, &Kinefin::JellyfinClient::serverReady);
    QVERIFY2(proxyReady.wait(20000), "the server probe through the forwarder never answered");
    QSignalSpy loggedIn(&proxyClient, &Kinefin::JellyfinClient::loginSucceeded);
    QVERIFY(proxyClient.authenticate(m_user, m_password));
    QVERIFY2(loggedIn.wait(20000), "the proxied login did not arrive");

    // The smallest demuxer cache the setting offers, so a starving link shows within seconds; an empty profile id keeps the controller's memory files off the disk
    QTemporaryDir dataDir;
    QVERIFY(dataDir.isValid());
    Kinefin::PlayQueue queue(&proxyClient);
    Kinefin::ClientSettings settings(dataDir.filePath(QStringLiteral("kinefin.conf")));
    QVERIFY(settings.setValue(QStringLiteral("video"), QStringLiteral("cache"), 10));
    Kinefin::PlayerController controller(proxyClient, queue, settings, QString());
    auto *engine = controller.findChild<Kinefin::PlaybackEngine *>();
    QVERIFY(engine);
    QVERIFY(engine->init({QStringLiteral("vo=null"), QStringLiteral("ao=null")}));
    QSignalSpy infos(&proxyClient, &Kinefin::JellyfinClient::playbackInfoReady);
    QSignalSpy lowered(&controller, &Kinefin::PlayerController::qualityLowered);
    QSignalSpy unavailable(&controller, &Kinefin::PlayerController::qualityUnavailable);
    const auto playing = [&controller] {
        return controller.active() && !controller.loading() && !controller.buffering()
               && !controller.paused() && controller.canSeek();
    };

    QVERIFY(controller.playItem(itemId, 0));
    QTRY_VERIFY_WITH_TIMEOUT(playing(), 30000);
    QCOMPARE(infos.size(), 1);
    const auto first = infos.first().first().value<Kinefin::PlaybackSource>();
    QCOMPARE(first.method, Kinefin::PlaybackSource::Method::Direct);
    QCOMPARE(int(first.url.port()), int(proxy.port()));
    QTRY_VERIFY_WITH_TIMEOUT(controller.positionTicks() > 3 * second, 20000);
    const qint64 movingFrom = controller.positionTicks();
    QTest::qWait(2000);
    QVERIFY2(controller.positionTicks() > movingFrom, "the unthrottled playback did not advance");

    // The middle of the film, where a UHD remux draws far more than in its opening minutes: with the demuxer cache at its smallest the link then starves within seconds of a throttle
    const qint64 middle = controller.durationTicks() / 2;
    QVERIFY2(middle > 600 * second, "the film is shorter than 20 minutes");
    // Without a renderer mpv plays the audio only and reads the video past on the wire, so the cache holds minutes of audio in 10 MiB: a cache of 1 MiB, filled again by that seek, runs dry in seconds
    engine->setMpvProperty(QStringLiteral("demuxer-max-bytes"), QStringLiteral("1MiB"));
    // mpv's own log for a run that needs the reason of an end-file error; it holds stream URLs with their key, so it is read redacted
    if (const QString log = qEnvironmentVariable("KINEFIN_LIVE_MPV_LOG"); !log.isEmpty()) {
        engine->setMpvProperty(QStringLiteral("log-file"), log);
        engine->setMpvProperty(QStringLiteral("msg-level"), QStringLiteral("all=v"));
    }
    controller.seek(middle);
    QTRY_VERIFY_WITH_TIMEOUT(controller.positionTicks() > middle && playing(), 60000);
    QTest::qWait(25000);

    // The link drops to 40 % of what the playback draws now: with the cache full that is the pace of the file at this position, which a variable-rate file keeps far from its average bitrate
    const qint64 drawnFrom = proxy.downstreamBytes();
    QTest::qWait(20000);
    const qint64 drawn = (proxy.downstreamBytes() - drawnFrom) * 8 / 20;
    const qint64 bytesPerSecond = drawn * 4 / 10 / 8;
    const qint64 beforeThrottle = controller.positionTicks();
    QElapsedTimer sinceThrottle;
    sinceThrottle.start();
    proxy.setDownstreamRate(bytesPerSecond);
    qInfo() << LIVE_FACT << "the playback drew" << drawn << "bit/s of the file's" << sourceBitrate
            << "average; throttled to" << bytesPerSecond * 8 << "bit/s at position"
            << beforeThrottle / second << "s; the fallback timings are the real ones";

    QElapsedTimer progressLog;
    progressLog.start();
    qint64 lastBytes = proxy.downstreamBytes();
    while ((lowered.isEmpty() && unavailable.isEmpty()) && sinceThrottle.elapsed() < 180000) {
        QTest::qWait(200);
        if (progressLog.elapsed() >= 10000) {
            qInfo() << LIVE_FACT << sinceThrottle.elapsed() / 1000 << "s: position"
                    << controller.positionTicks() / second << "s, buffering"
                    << controller.buffering() << "demuxer cache s"
                    << engine->mpvProperty(QStringLiteral("demuxer-cache-duration")).toDouble()
                    << "link bit/s"
                    << (proxy.downstreamBytes() - lastBytes) * 8 * 1000 / progressLog.elapsed();
            lastBytes = proxy.downstreamBytes();
            progressLog.restart();
        }
    }
    QVERIFY2(!lowered.isEmpty() || !unavailable.isEmpty(),
             "no fallback within 180 s of the throttle");
    QVERIFY2(unavailable.isEmpty(), "the fallback asked for a lower stream and was refused");
    const qint64 secondsToSwitch = sinceThrottle.elapsed() / 1000;
    const qint64 cap = lowered.first().first().toLongLong();
    qInfo() << LIVE_FACT << "qualityLowered cap" << cap << "bit/s," << secondsToSwitch
            << "s after the throttle";
    QCOMPARE(cap, Kinefin::lowerBitrate(sourceBitrate).value_or(0));
    // The link recovers: the lower stream is judged on its own, not on the throttle
    proxy.setDownstreamRate(0);

    // The lower stream is a transcode of the same item, offered without a second request
    QCOMPARE(infos.size(), 2);
    const auto lower = infos.at(1).first().value<Kinefin::PlaybackSource>();
    qInfo() << LIVE_FACT << "lower stream method" << int(lower.method) << "target bitrate"
            << lower.media.targetBitrate << "bit/s, source" << lower.media.source.bitrate << "path"
            << lower.url.path();
    QCOMPARE(lower.itemId, itemId);
    QCOMPARE(lower.method, Kinefin::PlaybackSource::Method::Transcode);
    QVERIFY(lower.playSessionId != first.playSessionId);
    QVERIFY2(lower.media.targetBitrate > 0 && lower.media.targetBitrate < sourceBitrate,
             "the transcode is not at a lower bitrate than the source");
    QCOMPARE(int(lower.url.port()), int(proxy.port()));
    QTRY_COMPARE_WITH_TIMEOUT(
        controller.playbackInfo().value(QStringLiteral("playMethod")).toString(),
        QStringLiteral("Transcode"), 20000);

    // Playing again near where it was: not before the throttle, and not further than the wall time allows
    QTRY_VERIFY_WITH_TIMEOUT(playing(), 90000);
    QTRY_VERIFY_WITH_TIMEOUT(controller.positionTicks() > 0, 20000);
    const qint64 after = controller.positionTicks();
    const qint64 wall = sinceThrottle.elapsed() / 1000;
    qInfo() << LIVE_FACT << "playing again at" << after / second << "s (before the throttle"
            << beforeThrottle / second << "s," << wall << "s of wall time since)";
    QVERIFY2(after >= beforeThrottle - 5 * second, "the position went back after the switch");
    QVERIFY2(after <= beforeThrottle + (wall + 30) * second, "the position jumped ahead");

    // The new play is on the server as a transcode
    QString method;
    const auto serverMethod = [&] {
        for (const auto &value : getDocument(client, QStringLiteral("/Sessions")).array()) {
            const QJsonObject session = value.toObject();
            if (session.value(QStringLiteral("DeviceId")).toString() == proxyDeviceId)
                method = session.value(QStringLiteral("PlayState"))
                             .toObject()
                             .value(QStringLiteral("PlayMethod"))
                             .toString();
        }
        return method;
    };
    QTRY_COMPARE_WITH_TIMEOUT(serverMethod(), QStringLiteral("Transcode"), 15000);
    qInfo() << LIVE_FACT << "server session PlayMethod after the switch:" << method;

    // The old stream's Stopped went out before the new stream's Start
    QTest::qWait(3000);
    const QStringList reports = proxy.reports();
    qInfo() << LIVE_FACT << "upstream Start and Stopped requests in order:" << reports;
    QVERIFY(reports.size() >= 3);
    QCOMPARE(reports.at(0), QStringLiteral("Start ") + first.playSessionId);
    QCOMPARE(reports.at(1), QStringLiteral("Stopped ") + first.playSessionId);
    QCOMPARE(reports.at(2), QStringLiteral("Start ") + lower.playSessionId);
    qInfo() << LIVE_FACT << "fallback steps taken:" << lowered.size();

    controller.stop();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.active(), 30000);
    QTRY_VERIFY_WITH_TIMEOUT(sessionSnapshot(client, proxyDeviceId, lower.playSessionId)
                                 .contains(QStringLiteral("\"nowPlayingType\":\"\"")),
                             15000);
    qInfo() << LIVE_FACT << "server session record after stop:"
            << sessionSnapshot(client, proxyDeviceId, lower.playSessionId);
    proxyClient.disconnectFromServer();
    client.disconnectFromServer();
}

void TestJellyfinLive::engineSurvivesALocalLogout()
{
    if (!m_live)
        QSKIP("no KINEFIN_LIVE_* environment, skipping the live checks");
    Kinefin::JellyfinClient client;
    QVERIFY(authenticate(client));
    const QString deviceId = m_deviceId;
    QString itemId;
    Kinefin::PlaybackSource source;
    QVERIFY2(findDirectPlayable(client, &itemId, &source),
             "no direct-playable movie in the first 20");

    Kinefin::PlaybackSession session;
    auto *engine = new Kinefin::PlaybackEngine(client, session, this);
    QVERIFY(engine->init({QStringLiteral("vo=null"), QStringLiteral("ao=null")}));
    Kinefin::PlaybackReporter reporter(session, client, [&engine] {
        return Kinefin::PlaybackReporter::PlayerState{.positionTicks = engine->knownPositionTicks(),
                                                      .canSeek = engine->canSeek()};
    });
    QSignalSpy loggedOut(&client, &Kinefin::JellyfinClient::credentialsCleared);
    QVERIFY(engine->open(source));
    QTRY_VERIFY_WITH_TIMEOUT(session.state() == Kinefin::PlaybackSession::State::Playing, 30000);
    recordAndAssertPlaying(client, deviceId, source, session, engine);
    QTest::qWait(1500);
    const qint64 beforeLogout = engine->positionTicks();

    // The local logout (the credential leaves memory; the server keeps the token's own record until its own death): playback must not depend on the reports or identity
    client.disconnectFromServer();
    QCOMPARE(loggedOut.size(), 1);
    QTest::qWait(1500);
    QVERIFY2(engine->positionTicks() > beforeLogout,
             "the engine stopped advancing after the local credential left memory");

    // The stop still closes the session machine; the server-side record is left as the server knows it, since no further report can carry a stopped state anywhere
    engine->stop();
    QTRY_VERIFY_WITH_TIMEOUT(session.state() == Kinefin::PlaybackSession::State::Idle, 30000);
    qInfo() << LIVE_FACT << "the playback kept running through the logout and stopped locally at"
            << engine->positionTicks() << "ticks";
    engine->deleteLater();
}

void TestJellyfinLive::subPathBaseUrlRound()
{
    if (!m_live)
        QSKIP("no KINEFIN_LIVE_* environment, skipping the live checks");

    const QByteArray prefix = QByteArrayLiteral("/jellyfin-proxy");
    FixtureProxy proxy(this);
    QVERIFY2(proxy.start(m_base, prefix), "the subpath forwarder could not listen");
    QUrl through = m_base;
    through.setHost(QStringLiteral("127.0.0.1"));
    through.setPort(int(proxy.port()));
    through.setPath(QString::fromUtf8(prefix));
    QVERIFY2(through.toString().endsWith(QStringLiteral("/jellyfin-proxy")),
             "the subpath base URL lost its prefix");

    // The whole client loop speaks to a base that carries a path: probe, identity, login
    Kinefin::JellyfinClient client;
    ++deviceSequence;
    const QString deviceId = QStringLiteral("kinefin-live-fiddle-%1").arg(deviceSequence);
    QVERIFY(client.setDeviceIdentity(QStringLiteral("Kinefin"), deviceId, QStringLiteral("0.0.1")));
    QVERIFY(client.connectToServer(through));
    QSignalSpy ready(&client, &Kinefin::JellyfinClient::serverReady);
    QVERIFY2(ready.wait(20000), "the public probe through the subpath base never answered");
    qInfo() << LIVE_FACT << "the client's own view of the subpath base:" << client.serverUrl();
    QSignalSpy loggedIn(&client, &Kinefin::JellyfinClient::loginSucceeded);
    QVERIFY(client.authenticate(m_user, m_password));
    QVERIFY2(loggedIn.wait(20000), "the login through the subpath base never arrived");
    QVERIFY(client.isAuthenticated());
    QVERIFY(!client.webSocketRequest().url().isEmpty());

    // The upgrade comes out of the component's own request and crosses the prefix
    QWebSocket *socket = openSocket(client);
    QVERIFY2(socket, "the QWebSocket handshake through the subpath base did not reach 101");
    QVERIFY(socketFirstExchange(socket));

    // One authenticated report through the prefix (the r1 shape, still 204)
    const QString movie = firstItemId(client);
    QVERIFY2(!movie.isEmpty(), "no movie in the library to report on");
    Kinefin::PlaybackReport start;
    start.kind = Kinefin::PlaybackReport::Kind::Start;
    fill(start, movie, QStringLiteral("kinefin-live-fiddle-psid-subpath"), qint64(0));
    QCOMPARE(postReport(client, start), 204);
    QTest::qWait(2000);
    const QString during = sessionSnapshot(client, deviceId, {});
    QString compact = during;
    // The snapshot keys the play by the device's session record only
    QVERIFY2(!compact.startsWith(QLatin1Char('(')), "no session record for the subpath device");
    qInfo() << LIVE_FACT << "the subpath session record:" << compact;
    Kinefin::PlaybackReport stopped;
    stopped.kind = Kinefin::PlaybackReport::Kind::Stopped;
    fill(stopped, movie, QStringLiteral("kinefin-live-fiddle-psid-subpath"), qint64(0));
    QCOMPARE(postReport(client, stopped), 204);
    QTest::qWait(2000);

    socket->close(QWebSocketProtocol::CloseCodeNormal);
    client.disconnectFromServer();
}

/* ----- endpoint live rounds ----- */

void TestJellyfinLive::authHeaderWireEcho()
{
    if (!m_live)
        QSKIP("no KINEFIN_LIVE_* environment, skipping the live checks");
    // A raw throw-away login carrying exactly the header the client sends (MediaBrowser, no Token yet); the echoed AccessToken then probes the accepted wire variants
    QNetworkAccessManager network;
    const QByteArray headerWithoutToken =
        QByteArrayLiteral("MediaBrowser Client=\"Kinefin\", Device=\"wire echo\", "
                          "DeviceId=\"kinefin-live-fiddle-wire\", Version=\"0.0.1\"");
    const auto probe = [&](const QString &path, const QByteArray &headerName,
                           const QByteArray &headerValue) {
        QUrl url = m_base;
        url.setPath(url.path() + path);
        QNetworkRequest request(url);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::ManualRedirectPolicy);
        if (!headerName.isEmpty())
            request.setRawHeader(headerName, headerValue);
        QNetworkReply *reply = network.get(request);
        QSignalSpy finished(reply, &QNetworkReply::finished);
        finished.wait(15000);
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();
        return status;
    };

    QUrl loginUrl = m_base;
    loginUrl.setPath(loginUrl.path() + QStringLiteral("/Users/AuthenticateByName"));
    QNetworkRequest loginRequest(loginUrl);
    loginRequest.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    loginRequest.setRawHeader(QByteArrayLiteral("Authorization"), headerWithoutToken);
    QNetworkReply *loginReply =
        network.post(loginRequest, QJsonDocument(QJsonObject{{QStringLiteral("Username"), m_user},
                                                             {QStringLiteral("Pw"), m_password}})
                                       .toJson(QJsonDocument::Compact));
    QSignalSpy done(loginReply, &QNetworkReply::finished);
    done.wait(15000);
    const int loginStatus = loginReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray loginBody = loginReply->readAll();
    loginReply->deleteLater();
    QCOMPARE(loginStatus, 200);
    const QJsonObject login = QJsonDocument::fromJson(loginBody).object();
    const QString token = login.value(QStringLiteral("AccessToken")).toString();
    QVERIFY2(!token.isEmpty(), "the raw login carried no AccessToken");
    qInfo() << LIVE_FACT << "AuthenticateByName 200 with the plain MediaBrowser header; keys:"
            << QStringList(login.keys());

    const QByteArray fullHeader = headerWithoutToken + QByteArrayLiteral(", Token=\"")
                                  + token.toUtf8() + QByteArrayLiteral("\"");
    QCOMPARE(probe(QStringLiteral("/Users/Me"), QByteArrayLiteral("Authorization"), fullHeader),
             200);
    qInfo() << LIVE_FACT << "/Users/Me answers 200 with the full MediaBrowser Token header";
    qInfo() << LIVE_FACT << "the X-Emby-Token header variant answered"
            << probe(QStringLiteral("/Users/Me"), QByteArrayLiteral("X-Emby-Token"),
                     token.toUtf8());
    qInfo() << LIVE_FACT << "the ?api_key= query variant answered"
            << probe(QStringLiteral("/Users/Me?api_key=") + token, {}, {});
    qInfo() << LIVE_FACT << "the ?ApiKey= query variant answered"
            << probe(QStringLiteral("/Users/Me?ApiKey=") + token, {}, {});
}

void TestJellyfinLive::browseListingsLive()
{
    if (!m_live)
        QSKIP("no KINEFIN_LIVE_* environment, skipping the live checks");
    Kinefin::JellyfinClient client;
    QVERIFY(authenticate(client));
    const QString userId = client.userId();
    QSignalSpy listFailed(&client, &Kinefin::JellyfinClient::listingFailed);

    // Real ids to work with: the first library, the first movies, a series
    const QJsonObject views = getJson(client, QStringLiteral("/UserViews"));
    const QJsonArray viewItems = views.value(QStringLiteral("Items")).toArray();
    QVERIFY2(!viewItems.isEmpty(), "no libraries on this server");
    const QString libraryId = viewItems.first().toObject().value(QStringLiteral("Id")).toString();
    qInfo() << LIVE_FACT << "the first library:"
            << viewItems.first().toObject().value(QStringLiteral("Name")).toString() << libraryId;

    const QJsonObject movies = getJson(
        client, QStringLiteral("/Items?IncludeItemTypes=Movie&Recursive=true&SortBy=Name&Limit=5"));
    const QJsonArray movieItems = movies.value(QStringLiteral("Items")).toArray();
    QVERIFY2(!movieItems.isEmpty(), "no movies on this server");
    const QString movieId = movieItems.first().toObject().value(QStringLiteral("Id")).toString();
    const QString imageTag = movieItems.first()
                                 .toObject()
                                 .value(QStringLiteral("ImageTags"))
                                 .toObject()
                                 .value(QStringLiteral("Primary"))
                                 .toString();
    qInfo() << LIVE_FACT << "the first movie:" << movieId << "primary image tag:" << imageTag;

    // The composed /Items query through the component
    Kinefin::ItemQuery query;
    query.parentId = libraryId;
    query.includeItemTypes = QStringLiteral("Movie,Series");
    query.sortBy = QStringLiteral("SortName");
    query.sortOrder = QStringLiteral("Descending");
    query.startIndex = 0;
    query.limit = 7;
    query.recursive = true;
    query.fields = {QStringLiteral("PrimaryImageAspectRatio"), QStringLiteral("Path"),
                    QStringLiteral("MediaSourceCount")};
    QSignalSpy itemsReady(&client, &Kinefin::JellyfinClient::itemsReady);
    QVERIFY(client.fetchItems(query));
    QVERIFY2(itemsReady.wait(15000), "the composed /Items query never answered");
    qInfo() << LIVE_FACT << "/Items compose:" << itemsReady.first().at(1).toInt() << "total,"
            << itemsReady.first().constFirst().value<QJsonArray>().size() << "on the page";
    QCOMPARE(listFailed.size(), 0);

    // The ids= tunnel: two ids resolve to themselves and nothing else
    Kinefin::ItemQuery idsQuery;
    const QString idA = movieItems.at(0).toObject().value(QStringLiteral("Id")).toString();
    const QString idB = movieItems.size() > 1
                            ? movieItems.at(1).toObject().value(QStringLiteral("Id")).toString()
                            : idA;
    idsQuery.itemIds = {idA, idB};
    QSignalSpy idsReady(&client, &Kinefin::JellyfinClient::itemsReady);
    QVERIFY(client.fetchItems(idsQuery));
    QVERIFY2(idsReady.wait(15000), "the ids= resolution never answered");
    const QJsonArray idItems = idsReady.first().constFirst().value<QJsonArray>();
    qInfo() << LIVE_FACT << "/Items?ids= (2 ids) answered" << idItems.size() << "items";
    QVERIFY2(idItems.size() >= 1 && idItems.size() <= 2,
             "the ids= tunnel returned items outside the requested set");

    // The home rows
    QSignalSpy resumeReady(&client, &Kinefin::JellyfinClient::resumeItemsReady);
    QVERIFY(client.fetchResumeItems(12));
    QVERIFY2(resumeReady.wait(15000), "the Resume row never answered");
    qInfo() << LIVE_FACT << "/UserItems/Resume:" << resumeReady.first().at(1).toInt() << "total,"
            << resumeReady.first().constFirst().value<QJsonArray>().size() << "rows";

    QSignalSpy latestReady(&client, &Kinefin::JellyfinClient::latestItemsReady);
    QVERIFY(client.fetchLatestItems(libraryId, 8));
    QVERIFY2(latestReady.wait(15000), "the Latest row never answered");
    const QJsonArray latest = latestReady.first().constFirst().value<QJsonArray>();
    qInfo() << LIVE_FACT << "/Items/Latest:" << latest.size() << "rows, first Type"
            << (latest.isEmpty()
                    ? QString()
                    : latest.first().toObject().value(QStringLiteral("Type")).toString());

    const QString cutoff = QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd"));
    QSignalSpy nextUpReady(&client, &Kinefin::JellyfinClient::nextUpReady);
    QVERIFY(client.fetchNextUp({}, libraryId, cutoff, false, 15));
    QVERIFY2(nextUpReady.wait(15000), "the NextUp row never answered");
    qInfo() << LIVE_FACT << "/Shows/NextUp cutoff" << cutoff
            << "rewatch=false:" << nextUpReady.first().constFirst().value<QJsonArray>().size()
            << "rows";
    QVERIFY(client.fetchNextUp({}, libraryId, cutoff, true, 15));
    QTRY_VERIFY_WITH_TIMEOUT(nextUpReady.size() >= 2, 15000);
    qInfo() << LIVE_FACT << "/Shows/NextUp rewatch=true:"
            << nextUpReady.constLast().constFirst().value<QJsonArray>().size() << "rows";

    // Series navigation
    const QJsonObject series = getJson(
        client,
        QStringLiteral("/Items?IncludeItemTypes=Series&Recursive=true&SortBy=Name&Limit=1"));
    const QJsonArray seriesItems = series.value(QStringLiteral("Items")).toArray();
    QVERIFY2(!seriesItems.isEmpty(), "no series on this server");
    const QString seriesId = seriesItems.first().toObject().value(QStringLiteral("Id")).toString();
    QSignalSpy seasonsReady(&client, &Kinefin::JellyfinClient::seasonsReady);
    QVERIFY(client.fetchSeasons(seriesId));
    QVERIFY2(seasonsReady.wait(15000), "the Seasons listing never answered");
    const QJsonArray seasons = seasonsReady.first().constFirst().value<QJsonArray>();
    qInfo() << LIVE_FACT << "/Shows/{id}/Seasons:" << seasons.size() << "seasons on the page of"
            << seasonsReady.first().at(1).toInt();
    const QString seasonId =
        seasons.isEmpty() ? QString()
                          : seasons.first().toObject().value(QStringLiteral("Id")).toString();
    QSignalSpy episodesReady(&client, &Kinefin::JellyfinClient::episodesReady);
    QVERIFY(client.fetchEpisodes(seriesId, seasonId, 0, 10));
    QVERIFY2(episodesReady.wait(15000), "the Episodes listing never answered");
    qInfo() << LIVE_FACT << "/Shows/{id}/Episodes (first season, page of 10):"
            << episodesReady.first().constFirst().value<QJsonArray>().size() << "rows of"
            << episodesReady.first().at(1).toInt();
    QVERIFY(client.fetchEpisodes(seriesId, {}, -1, -1));
    QTRY_VERIFY_WITH_TIMEOUT(episodesReady.size() >= 2, 15000);
    qInfo() << LIVE_FACT << "/Shows/{id}/Episodes (every season, unpaged):"
            << episodesReady.constLast().constFirst().value<QJsonArray>().size() << "rows";

    // Similar: the movie rail, the Episode edge, the unknown-id edge
    QSignalSpy similarReady(&client, &Kinefin::JellyfinClient::similarItemsReady);
    QVERIFY(client.fetchSimilarItems(movieId, 12));
    QVERIFY2(similarReady.wait(15000), "the Similar listing never answered");
    qInfo() << LIVE_FACT << "/Items/{id}/Similar:" << similarReady.first().at(1).toInt() << "total,"
            << similarReady.first().constFirst().value<QJsonArray>().size() << "rows";
    const QJsonObject episodeProbe =
        getJson(client, QStringLiteral("/Items?IncludeItemTypes=Episode&Recursive=true&Limit=1"));
    const QJsonArray episodeItems = episodeProbe.value(QStringLiteral("Items")).toArray();
    if (!episodeItems.isEmpty()) {
        QVERIFY(client.fetchSimilarItems(
            episodeItems.first().toObject().value(QStringLiteral("Id")).toString(), 5));
        QTRY_VERIFY_WITH_TIMEOUT(similarReady.size() >= 2, 15000);
        qInfo() << LIVE_FACT << "Similar of an Episode answers"
                << similarReady.constLast().constFirst().value<QJsonArray>().size()
                << "rows (upstream: an empty envelope)";
    } else {
        qInfo() << LIVE_FACT << "no episode on this server; the Episode Similar edge is skipped";
    }
    QVERIFY(client.fetchSimilarItems(QStringLiteral("deadbeefdeadbeefdeadbeefdeadbeef"), 5));
    QTRY_VERIFY_WITH_TIMEOUT(listFailed.size() >= 1, 15000);
    qInfo() << LIVE_FACT << "Similar of a shape-valid unknown id answers"
            << listFailed.constLast().at(1).toInt() << listFailed.constLast().at(2).toString();

    // Genres and the browse filters
    QSignalSpy genresReady(&client, &Kinefin::JellyfinClient::genresReady);
    QVERIFY(client.fetchGenres(libraryId, 0, 30));
    QVERIFY2(genresReady.wait(15000), "the Genres listing never answered");
    const QJsonArray genres = genresReady.first().constFirst().value<QJsonArray>();
    qInfo() << LIVE_FACT << "/Genres:" << genres.size() << "of" << genresReady.first().at(1).toInt()
            << "total; first"
            << (genres.isEmpty()
                    ? QString()
                    : genres.first().toObject().value(QStringLiteral("Name")).toString());
    QSignalSpy filtersReady(&client, &Kinefin::JellyfinClient::filtersReady);
    QVERIFY(client.fetchFilters(libraryId));
    QVERIFY2(filtersReady.wait(15000), "the Filters2 listing never answered");
    const QJsonObject filters = filtersReady.first().constFirst().value<QJsonObject>();
    const QJsonArray audioLanguages = filters.value(QStringLiteral("AudioLanguages")).toArray();
    qInfo() << LIVE_FACT << "/Items/Filters2: Genres"
            << filters.value(QStringLiteral("Genres")).toArray().size() << "Tags"
            << filters.value(QStringLiteral("Tags")).toArray().size() << "AudioLanguages"
            << audioLanguages.size() << "first language row:"
            << QString::fromUtf8(QJsonDocument(audioLanguages.isEmpty()
                                                   ? QJsonObject()
                                                   : audioLanguages.first().toObject())
                                     .toJson(QJsonDocument::Compact));

    // Sessions: the plain set, the controllable subset, and the substitution probe
    QSignalSpy sessionsReady(&client, &Kinefin::JellyfinClient::sessionsReady);
    QVERIFY(client.fetchSessions(false));
    QVERIFY2(sessionsReady.wait(15000), "the /Sessions listing never answered");
    qInfo() << LIVE_FACT << "/Sessions (non-admin, plain):"
            << sessionsReady.first().constFirst().value<QJsonArray>().size() << "entries";
    QVERIFY(client.fetchSessions(true));
    QTRY_VERIFY_WITH_TIMEOUT(sessionsReady.size() >= 2, 15000);
    qInfo() << LIVE_FACT << "/Sessions?controllableByUserId=<self> (non-admin):"
            << sessionsReady.constLast().constFirst().value<QJsonArray>().size() << "entries";
    QByteArray foreignBody;
    const int foreignStatus = probeRaw(
        client, QStringLiteral("/Sessions?controllableByUserId=11111111111111111111111111111111"),
        &foreignBody, true);
    qInfo() << LIVE_FACT << "non-admin foreign controllableByUserId answered" << foreignStatus
            << "with" << QJsonDocument::fromJson(foreignBody).array().size()
            << "entries (the substitution fact)";

    // The token-free image route, unauthenticated
    const QUrl image = client.imageUrl(movieId, QStringLiteral("Primary"), 360, imageTag);
    QVERIFY2(!image.isEmpty(), "the image URL builder refused a live-valid input");
    QVERIFY(!image.toString().contains(QLatin1String("Token"), Qt::CaseInsensitive));
    QString imagePath = image.path();
    if (image.hasQuery())
        imagePath += QLatin1Char('?') + image.query();
    QString contentType;
    QByteArray imageBytes;
    const int imageStatus = probeRaw(client, imagePath, &imageBytes, false, &contentType);
    qInfo() << LIVE_FACT << "the unauthenticated image GET:" << imageStatus << contentType
            << imageBytes.size() << "bytes; url" << image.toString();
    QCOMPARE(imageStatus, 200);
    QVERIFY2(imageBytes.size() > 100, "the unauthenticated image GET returned no real bytes");
    const QUrl webp = client.imageUrl(movieId, QStringLiteral("Primary"), 360, imageTag, 85,
                                      QStringLiteral("webp"));
    QString webpType;
    QByteArray webpBytes;
    QString webpPath = webp.path();
    if (webp.hasQuery())
        webpPath += QLatin1Char('?') + webp.query();
    QCOMPARE(probeRaw(client, webpPath, &webpBytes, false, &webpType), 200);
    qInfo() << LIVE_FACT << "the format=webp variant content-type:" << webpType << webpBytes.size()
            << "bytes";
    // A wrong tag is recorded, not asserted: 12.x may serve the fallback image instead
    const QUrl wrongTag =
        client.imageUrl(movieId, QStringLiteral("Primary"), 360, QStringLiteral("zz"));
    QString wrongPath = wrongTag.path();
    if (wrongTag.hasQuery())
        wrongPath += QLatin1Char('?') + wrongTag.query();
    QByteArray wrongBytes;
    QString wrongType;
    qInfo() << LIVE_FACT << "the wrong-tag image GET answered"
            << probeRaw(client, wrongPath, &wrongBytes, false, &wrongType) << wrongType;

    // The server's echo of an invalid query member (the client-side refusals mirror it)
    QByteArray invalidBody;
    const int invalidStatus = probeRaw(
        client, QStringLiteral("/Items?fields=NotAField&userId=") + userId, &invalidBody, true);
    qInfo() << LIVE_FACT << "GET /Items with fields=NotAField answered" << invalidStatus
            << QString::fromUtf8(invalidBody.left(300));
    QCOMPARE(listFailed.size(), 1);
    client.disconnectFromServer();
}

void TestJellyfinLive::sessionsRoleVisibility()
{
    if (!m_live)
        QSKIP("no KINEFIN_LIVE_* environment, skipping the live checks");
    if (!m_adminLive)
        QSKIP("no KINEFIN_LIVE_ADMIN_* environment: the admin-vs-non-admin /Sessions row "
              "needs an admin login");
    Kinefin::JellyfinClient userClient;
    QVERIFY(authenticate(userClient));
    const QJsonArray userSessions = getDocument(userClient, QStringLiteral("/Sessions")).array();
    Kinefin::JellyfinClient adminClient;
    QVERIFY(authenticateAs(adminClient, m_adminUser, m_adminPassword));
    const QJsonArray adminSessions = getDocument(adminClient, QStringLiteral("/Sessions")).array();
    qInfo() << LIVE_FACT << "/Sessions visible sets: non-admin" << userSessions.size()
            << "entries, admin" << adminSessions.size() << "entries";
    QVERIFY2(adminSessions.size() >= userSessions.size(),
             "the non-admin saw more sessions than the admin");
    // The foreign controllableByUserId through both roles: non-admin substituted, admin literal
    QByteArray userForeign;
    QByteArray adminForeign;
    const int userForeignStatus =
        probeRaw(userClient,
                 QStringLiteral("/Sessions?controllableByUserId=11111111111111111111111111111111"),
                 &userForeign, true);
    const int adminForeignStatus =
        probeRaw(adminClient,
                 QStringLiteral("/Sessions?controllableByUserId=11111111111111111111111111111111"),
                 &adminForeign, true);
    qInfo() << LIVE_FACT << "foreign controllableByUserId: non-admin" << userForeignStatus
            << QJsonDocument::fromJson(userForeign).array().size() << "entries, admin"
            << adminForeignStatus << QJsonDocument::fromJson(adminForeign).array().size()
            << "entries";
    userClient.disconnectFromServer();
    adminClient.disconnectFromServer();
}

void TestJellyfinLive::trickplayLayoutProbe()
{
    if (!m_live)
        QSKIP("no KINEFIN_LIVE_* environment, skipping the live checks");
    Kinefin::JellyfinClient client;
    QVERIFY(authenticate(client));
    const QJsonObject movies = getJson(
        client,
        QStringLiteral("/Items?IncludeItemTypes=Movie&Recursive=true&Limit=20&Fields=Trickplay"));
    QString itemId;
    QString width;
    QJsonObject trickplay;
    for (const auto &value : movies.value(QStringLiteral("Items")).toArray()) {
        const QJsonObject item = value.toObject();
        const QJsonObject trick = item.value(QStringLiteral("Trickplay")).toObject();
        if (trick.isEmpty())
            continue;
        itemId = item.value(QStringLiteral("Id")).toString();
        trickplay = trick;
        width = trick.keys().constFirst();
        break;
    }
    if (itemId.isEmpty())
        QSKIP("no trickplay scans on this server (trickplay needs the server's image scans)");
    qInfo() << LIVE_FACT << "the Trickplay field of" << itemId << ":"
            << QString::fromUtf8(QJsonDocument(trickplay).toJson(QJsonDocument::Compact));
    QByteArray manifest;
    const int manifestStatus =
        probeRaw(client,
                 QStringLiteral("/Videos/") + itemId + QStringLiteral("/Trickplay/") + width
                     + QStringLiteral("/tiles.m3u8"),
                 &manifest, true);
    QCOMPARE(manifestStatus, 200);
    qInfo() << LIVE_FACT << "tiles.m3u8 (width" << width
            << ") body:" << QString::fromUtf8(manifest.left(600));
    QByteArray tile;
    const int tileStatus =
        probeRaw(client,
                 QStringLiteral("/Videos/") + itemId + QStringLiteral("/Trickplay/") + width
                     + QStringLiteral("/0.jpg"),
                 &tile, true);
    QCOMPARE(tileStatus, 200);
    const QImage image = QImage::fromData(tile);
    QVERIFY2(!image.isNull(), "the trickplay tile did not decode as an image");
    qInfo() << LIVE_FACT << "tile 0.jpg:" << tile.size() << "bytes," << image.width() << "x"
            << image.height() << "pixels";
    client.disconnectFromServer();
}

void TestJellyfinLive::subtitleDeliveryProbe()
{
    if (!m_live)
        QSKIP("no KINEFIN_LIVE_* environment, skipping the live checks");
    Kinefin::JellyfinClient client;
    QVERIFY(authenticate(client));
    QNetworkAccessManager network;
    const auto send = [&](const QString &path, const QUrlQuery &query, const QJsonObject *body) {
        QUrl url = client.serverUrl();
        url.setPath(url.path() + path);
        url.setQuery(query);
        QNetworkRequest request(url);
        request.setRawHeader(QByteArrayLiteral("Authorization"), client.streamAuthorization());
        QNetworkReply *reply = nullptr;
        if (body) {
            request.setHeader(QNetworkRequest::ContentTypeHeader,
                              QStringLiteral("application/json"));
            reply = network.post(request, QJsonDocument(*body).toJson(QJsonDocument::Compact));
        } else {
            reply = network.get(request);
        }
        QSignalSpy done(reply, &QNetworkReply::finished);
        done.wait(30000);
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject answer = QJsonDocument::fromJson(reply->readAll()).object();
        reply->deleteLater();
        return std::pair<int, QJsonObject>{status, answer};
    };
    // A movie MKV with at least two embedded SubRip streams
    QUrlQuery listing;
    listing.addQueryItem(QStringLiteral("userId"), client.userId());
    listing.addQueryItem(QStringLiteral("includeItemTypes"), QStringLiteral("Movie"));
    listing.addQueryItem(QStringLiteral("recursive"), QStringLiteral("true"));
    listing.addQueryItem(QStringLiteral("limit"), QStringLiteral("400"));
    listing.addQueryItem(QStringLiteral("fields"), QStringLiteral("MediaStreams,Container"));
    const auto [listStatus, list] = send(QStringLiteral("/Items"), listing, nullptr);
    QCOMPARE(listStatus, 200);
    QString itemId;
    for (const QJsonValue &value : list.value(QStringLiteral("Items")).toArray()) {
        const QJsonObject item = value.toObject();
        int embedded = 0;
        for (const QJsonValue &stream : item.value(QStringLiteral("MediaStreams")).toArray()) {
            const QJsonObject s = stream.toObject();
            if (s.value(QStringLiteral("Type")).toString() == QLatin1String("Subtitle")
                && !s.value(QStringLiteral("IsExternal")).toBool()
                && s.value(QStringLiteral("Codec")).toString() == QLatin1String("subrip"))
                ++embedded;
        }
        if (embedded >= 2
            && item.value(QStringLiteral("Container")).toString().contains(QLatin1String("mkv"))) {
            itemId = item.value(QStringLiteral("Id")).toString();
            break;
        }
    }
    QVERIFY2(!itemId.isEmpty(), "no movie MKV with two embedded SubRip streams");

    const QJsonObject body{{QStringLiteral("UserId"), client.userId()},
                           {QStringLiteral("MaxStreamingBitrate"), 2'000'000},
                           {QStringLiteral("DeviceProfile"), Kinefin::buildDeviceProfile({})}};
    const auto [status, answer] =
        send(QStringLiteral("/Items/") + itemId + QStringLiteral("/PlaybackInfo"), {}, &body);
    QCOMPARE(status, 200);
    const QJsonObject source =
        answer.value(QStringLiteral("MediaSources")).toArray().first().toObject();
    QVERIFY2(!source.value(QStringLiteral("TranscodingUrl")).toString().isEmpty(),
             "the cap did not make the server transcode");
    const QUrlQuery transcode(QUrl(source.value(QStringLiteral("TranscodingUrl")).toString()));
    qInfo() << LIVE_FACT << "subtitle delivery: TranscodingUrl SubtitleMethod"
            << transcode.queryItemValue(QStringLiteral("SubtitleMethod")) << "SubtitleStreamIndex"
            << transcode.queryItemValue(QStringLiteral("SubtitleStreamIndex")) << "SubtitleCodec"
            << transcode.queryItemValue(QStringLiteral("SubtitleCodec"));
    int embeddedExternal = 0;
    for (const QJsonValue &value : source.value(QStringLiteral("MediaStreams")).toArray()) {
        const QJsonObject stream = value.toObject();
        if (stream.value(QStringLiteral("Type")).toString() != QLatin1String("Subtitle"))
            continue;
        const QString deliveryUrl = stream.value(QStringLiteral("DeliveryUrl")).toString();
        // The URL's shape only: the ids masked, the query keys without values
        QString shape = QUrl(deliveryUrl).path();
        shape.replace(itemId, QStringLiteral("<item>"), Qt::CaseInsensitive);
        shape.replace(source.value(QStringLiteral("Id")).toString(), QStringLiteral("<source>"),
                      Qt::CaseInsensitive);
        QStringList queryKeys;
        for (const auto &[key, valueText] : QUrlQuery(QUrl(deliveryUrl)).queryItems())
            queryKeys << key;
        const bool external = stream.value(QStringLiteral("IsExternal")).toBool();
        const QString method = stream.value(QStringLiteral("DeliveryMethod")).toString();
        qInfo() << LIVE_FACT << "subtitle stream" << stream.value(QStringLiteral("Index")).toInt()
                << "codec" << stream.value(QStringLiteral("Codec")).toString() << "external"
                << external << "text"
                << stream.value(QStringLiteral("IsTextSubtitleStream")).toBool() << "method"
                << method << "url" << shape << "query keys" << queryKeys;
        if (!external && method == QLatin1String("External") && !deliveryUrl.isEmpty())
            ++embeddedExternal;
    }
    qInfo() << LIVE_FACT
            << "embedded text streams delivered External with a URL:" << embeddedExternal;
    // What the client's own parser keeps of the same answer
    QSignalSpy ready(&client, &Kinefin::JellyfinClient::playbackInfoReady);
    QVERIFY(
        client.requestPlaybackInfo(itemId, qint64(0), Kinefin::buildDeviceProfile({}), 2'000'000));
    QVERIFY(ready.wait(30000));
    const auto parsed = ready.first().first().value<Kinefin::PlaybackSource>();
    qInfo() << LIVE_FACT << "the parser keeps" << parsed.externalSubtitles.size()
            << "server subtitles, method transcode"
            << (parsed.method == Kinefin::PlaybackSource::Method::Transcode);
    QCOMPARE(parsed.externalSubtitles.size(), embeddedExternal + 1);
    // The server extracts an embedded stream when it is asked for: the time to the file and its size are the facts
    const Kinefin::ExternalSubtitle &embedded = parsed.externalSubtitles.constLast();
    QSignalSpy downloaded(&client, &Kinefin::JellyfinClient::externalSubtitleDownloaded);
    QSignalSpy failed(&client, &Kinefin::JellyfinClient::externalSubtitleFailed);
    QElapsedTimer fetch;
    fetch.start();
    QVERIFY(client.downloadExternalSubtitle(embedded.url));
    QTRY_VERIFY_WITH_TIMEOUT(!downloaded.isEmpty() || !failed.isEmpty(), 120000);
    const QByteArray bytes =
        downloaded.isEmpty() ? QByteArray() : downloaded.first().at(1).toByteArray();
    qInfo() << LIVE_FACT << "embedded stream" << embedded.index << "format" << embedded.format
            << "downloaded" << !downloaded.isEmpty() << "in ms" << fetch.elapsed() << "bytes"
            << bytes.size() << "arrow lines" << bytes.count("-->");
    QCOMPARE(failed.size(), 0);
    QVERIFY(bytes.contains("-->"));
    client.disconnectFromServer();
}

void TestJellyfinLive::playbackInfoDefaultsProbe()
{
    if (!m_live)
        QSKIP("no KINEFIN_LIVE_* environment, skipping the live checks");
    Kinefin::JellyfinClient client;
    QVERIFY(authenticate(client));
    const QString hdrId = hdrItemId(client);
    QVERIFY2(!hdrId.isEmpty(), "no HDR/DoVi movie for the defaults probe");
    const QString userId = client.userId();
    const QJsonObject profile = Kinefin::buildDeviceProfile({});

    QNetworkAccessManager network;
    const auto postForBody = [&](const QString &path, const QJsonObject &body) {
        QUrl url = client.serverUrl();
        url.setPath(url.path() + path);
        QNetworkRequest request(url);
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        const QByteArray token = client.streamAuthorization();
        if (!token.isEmpty())
            request.setRawHeader(QByteArrayLiteral("Authorization"), token);
        QNetworkReply *reply =
            network.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
        QSignalSpy done(reply, &QNetworkReply::finished);
        done.wait(20000);
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray bytes = reply->readAll();
        reply->deleteLater();
        return std::pair<int, QJsonObject>{status, QJsonDocument::fromJson(bytes).object()};
    };
    const auto recordShape = [](const QString &label, const QJsonObject &answer) {
        const QJsonArray sources = answer.value(QStringLiteral("MediaSources")).toArray();
        const QString transcodingUrl =
            sources.isEmpty()
                ? QString()
                : sources.first().toObject().value(QStringLiteral("TranscodingUrl")).toString();
        const QString bitrate = transcodingUrl.isEmpty()
                                    ? QString()
                                    : QUrlQuery(QUrl(transcodingUrl))
                                          .queryItemValue(QStringLiteral("MaxStreamingBitrate"));
        qInfo() << LIVE_FACT << label << ": MediaSources" << sources.size() << "PlaySessionId"
                << !answer.value(QStringLiteral("PlaySessionId")).toString().isEmpty()
                << "TranscodingUrl" << !transcodingUrl.isEmpty() << "its MaxStreamingBitrate"
                << bitrate;
    };

    const QString route = QStringLiteral("/Items/") + hdrId + QStringLiteral("/PlaybackInfo");
    const auto [defaultsStatus, withDefaults] =
        postForBody(route, QJsonObject{{QStringLiteral("UserId"), userId},
                                       {QStringLiteral("DeviceProfile"), profile}});
    QCOMPARE(defaultsStatus, 200);
    recordShape(QStringLiteral("the defaults body (no MaxStreamingBitrate)"), withDefaults);
    const auto [lowStatus, withLow] =
        postForBody(route, QJsonObject{{QStringLiteral("UserId"), userId},
                                       {QStringLiteral("MaxStreamingBitrate"), 500000},
                                       {QStringLiteral("DeviceProfile"), profile}});
    QCOMPARE(lowStatus, 200);
    recordShape(QStringLiteral("the 500 kbps ceiling body"), withLow);
    const auto [bareStatus, bare] =
        postForBody(route, QJsonObject{{QStringLiteral("UserId"), userId}});
    QCOMPARE(bareStatus, 200);
    recordShape(QStringLiteral("no DeviceProfile at all"), bare);
    client.disconnectFromServer();
}

void TestJellyfinLive::playlistEdits()
{
    if (!m_live)
        QSKIP("no KINEFIN_LIVE_* environment, skipping the live checks");
    Kinefin::JellyfinClient client;
    QVERIFY(authenticate(client));
    const QJsonArray movies =
        getJson(client,
                QStringLiteral("/Items?IncludeItemTypes=Movie&Recursive=true&SortBy=Name&Limit=3"))
            .value(QStringLiteral("Items"))
            .toArray();
    if (movies.size() < 3)
        QSKIP("fewer than three movies in the user's libraries");
    QStringList itemIds;
    for (const QJsonValue &movie : movies)
        itemIds.append(movie.toObject().value(QStringLiteral("Id")).toString().toLower());

    QSignalSpy written(&client, &Kinefin::JellyfinClient::playlistWriteSucceeded);
    QSignalSpy failed(&client, &Kinefin::JellyfinClient::playlistWriteFailed);
    QSignalSpy read(&client, &Kinefin::JellyfinClient::playlistEntriesReady);
    // The `writes`-th write succeeded and none failed
    const auto settled = [&](qsizetype writes) {
        QElapsedTimer timer;
        timer.start();
        while (written.size() < writes && failed.isEmpty() && timer.elapsed() < 15000)
            QTest::qWait(50);
        return written.size() >= writes && failed.isEmpty();
    };
    const auto field = [](const QJsonArray &items, const QString &key) {
        QStringList values;
        for (const QJsonValue &item : items)
            values.append(item.toObject().value(key).toString().toLower());
        return values;
    };

    QVERIFY(client.createPlaylist(QStringLiteral("Kinefin live check"), itemIds));
    QVERIFY2(settled(1), "the playlist was not created");
    const QString playlist = written.constLast().at(2).toString();
    QVERIFY(Kinefin::isValidJellyfinId(playlist));
    bool deleted = false;
    const auto cleanup = qScopeGuard([&] {
        if (!deleted)
            qInfo() << LIVE_FACT << "cleanup DELETE of the live check playlist answered"
                    << deleteRequest(client, QStringLiteral("/Items/") + playlist);
    });
    const auto readEntries = [&](QJsonArray *items) {
        read.clear();
        if (!client.fetchPlaylistEntries(playlist, 0, 100) || !read.wait(15000))
            return false;
        *items = read.constFirst().at(0).value<QJsonArray>();
        return true;
    };

    QJsonArray items;
    QVERIFY(readEntries(&items));
    QCOMPARE(field(items, QStringLiteral("Id")), itemIds);
    const QString firstEntry = field(items, QStringLiteral("PlaylistItemId")).constFirst();
    QVERIFY(Kinefin::isValidJellyfinId(firstEntry));

    // The model assumes newIndex is the final index of the entry: 0,1,2 then reads back as 1,2,0
    QVERIFY(client.movePlaylistEntry(playlist, firstEntry, 2));
    QVERIFY2(settled(2), "the move failed");
    QVERIFY(readEntries(&items));
    QList<qsizetype> order;
    for (const QString &id : field(items, QStringLiteral("Id")))
        order.append(itemIds.indexOf(id));
    qInfo() << LIVE_FACT
            << "the first of three entries moved to newIndex 2 reads back as the original positions"
            << order << "(1,2,0: newIndex is the final index; 1,0,2: the entry goes before it)";
    QCOMPARE(order, (QList<qsizetype>{1, 2, 0}));

    const QString itemPath =
        QStringLiteral("/Items/") + playlist + QStringLiteral("?userId=") + client.userId();
    const QJsonObject item = getJson(client, itemPath);
    const QJsonValue canDelete = item.value(QStringLiteral("CanDelete"));
    qInfo() << LIVE_FACT << "GET /Items/{playlist} for its owner: Type"
            << item.value(QStringLiteral("Type")).toString() << "CanDelete" << canDelete;
    QVERIFY2(canDelete.isBool() && canDelete.toBool(), "the owner's CanDelete is not true");

    QVERIFY(client.deletePlaylist(playlist));
    QVERIFY2(settled(3), "the delete failed");
    const int status = probeRaw(client, itemPath, nullptr, true);
    deleted = status != 200;
    qInfo() << LIVE_FACT << "GET /Items/{playlist} after the delete answers" << status;
    QVERIFY2(deleted, "the playlist is still there after the delete");
    client.disconnectFromServer();
}

void TestJellyfinLive::syncPlayProtocol()
{
    if (!m_live)
        QSKIP("no KINEFIN_LIVE_* environment, skipping the live checks");

    // One session of the user: its client, its raw socket once opened, and what that socket received (keep-alives left out) with how much of it is printed
    struct Member {
        QString name;
        Kinefin::JellyfinClient client;
        QWebSocket *socket = nullptr;
        QStringList frames;
        qsizetype printed = 0;
    };
    Member a;
    a.name = QStringLiteral("A");
    Member b;
    b.name = QStringLiteral("B");
    QVERIFY(authenticate(a.client));
    QVERIFY(authenticate(b.client));
    const auto cleanup = qScopeGuard([&] {
        for (Member *member : {&a, &b}) {
            postJson(member->client, QStringLiteral("/SyncPlay/Leave"), {});
            delete member->socket;
            member->client.disconnectFromServer();
        }
    });

    // The server closes a silent socket at 60 s: answer its ForceKeepAlive and beat every 25 s
    const auto openSocketOf = [](Member &member) {
        delete member.socket;
        auto *socket =
            new QWebSocket(QStringLiteral("Kinefin live check"), QWebSocketProtocol::VersionLatest);
        member.socket = socket;
        const QString keepAlive = QStringLiteral("{\"MessageType\":\"KeepAlive\"}");
        QObject::connect(socket, &QWebSocket::textMessageReceived, socket,
                         [&member, socket, keepAlive](const QString &frame) {
                             if (frame.contains(QLatin1String("ForceKeepAlive")))
                                 socket->sendTextMessage(keepAlive);
                             else if (!frame.contains(QLatin1String("\"KeepAlive\"")))
                                 member.frames.append(frame);
                         });
        auto *beat = new QTimer(socket);
        QObject::connect(beat, &QTimer::timeout, socket,
                         [socket, keepAlive] { socket->sendTextMessage(keepAlive); });
        beat->start(25000);
        QSignalSpy connected(socket, &QWebSocket::connected);
        socket->open(member.client.webSocketRequest());
        return connected.wait(15000);
    };
    // The frames since the previous step, in arrival order (the order is one of the facts)
    const auto settle = [&](const char *step, int ms = 2500) {
        QTest::qWait(ms);
        for (Member *member : {&a, &b}) {
            for (; member->printed < member->frames.size(); ++member->printed)
                qInfo().noquote() << LIVE_FACT << step << member->name
                                  << "frame:" << member->frames.at(member->printed);
        }
    };
    const auto post = [this](Member &member, const QString &route, const QJsonObject &body) {
        QByteArray answer;
        const int status =
            postJson(member.client, QStringLiteral("/SyncPlay/") + route, body, &answer);
        qInfo().noquote() << LIVE_FACT << member.name << QStringLiteral("POST /SyncPlay/") + route
                          << QJsonDocument(body).toJson(QJsonDocument::Compact) << "->" << status
                          << QString::fromUtf8(answer.left(400));
        return status;
    };
    const auto listGroups = [this](Member &member, const char *step) {
        const QJsonArray groups =
            getDocument(member.client, QStringLiteral("/SyncPlay/List")).array();
        qInfo().noquote() << LIVE_FACT << step << member.name << "GET /SyncPlay/List:"
                          << QJsonDocument(groups).toJson(QJsonDocument::Compact);
        return groups;
    };
    const auto readiness = [](qint64 ticks, bool playing, const QString &entry) {
        return QJsonObject{
            {QStringLiteral("When"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
            {QStringLiteral("PositionTicks"), ticks},
            {QStringLiteral("IsPlaying"), playing},
            {QStringLiteral("PlaylistItemId"), entry}};
    };
    // The playing entry of the latest PlayQueue update the member received
    const auto playingEntry = [](const Member &member) {
        for (qsizetype i = member.frames.size() - 1; i >= 0; --i) {
            const QJsonObject update = QJsonDocument::fromJson(member.frames.at(i).toUtf8())
                                           .object()
                                           .value(QStringLiteral("Data"))
                                           .toObject();
            if (update.value(QStringLiteral("Type")).toString() != QLatin1String("PlayQueue"))
                continue;
            const QJsonObject queue = update.value(QStringLiteral("Data")).toObject();
            const QJsonArray playlist = queue.value(QStringLiteral("Playlist")).toArray();
            const int index = queue.value(QStringLiteral("PlayingItemIndex")).toInt(-1);
            if (index < 0 || index >= playlist.size())
                return QString();
            return playlist.at(index).toObject().value(QStringLiteral("PlaylistItemId")).toString();
        }
        return QString();
    };

    qInfo() << LIVE_FACT << "/Users/Me Policy.SyncPlayAccess:"
            << fetchMe(a.client)
                   .value(QStringLiteral("Policy"))
                   .toObject()
                   .value(QStringLiteral("SyncPlayAccess"));
    QByteArray utc;
    const int utcStatus = probeRaw(a.client, QStringLiteral("/GetUtcTime"), &utc, true);
    qInfo().noquote() << LIVE_FACT << "GET /GetUtcTime ->" << utcStatus << QString::fromUtf8(utc);
    const QString movie = firstItemId(a.client);
    QVERIFY2(!movie.isEmpty(), "no movie in the library to queue");
    const QString groupName = QStringLiteral("Kinefin live check %1").arg(deviceSequence);
    const auto groupIdIn = [&](const QJsonArray &groups) {
        for (const QJsonValue &group : groups) {
            if (group.toObject().value(QStringLiteral("GroupName")).toString() == groupName)
                return group.toObject().value(QStringLiteral("GroupId")).toString();
        }
        return QString();
    };

    // A session with no socket: can it create a group at all
    post(a, QStringLiteral("New"),
         {{QStringLiteral("GroupName"), QString(groupName + QStringLiteral(" without socket"))}});
    listGroups(b, "new-without-socket");
    post(a, QStringLiteral("Leave"), {});
    listGroups(b, "new-without-socket-left");

    QVERIFY2(openSocketOf(a), "A's socket did not open");
    settle("a-socket-open");
    qInfo() << LIVE_FACT << "A capabilities with media control ->"
            << postJson(
                   a.client, QStringLiteral("/Sessions/Capabilities/Full"),
                   {{QStringLiteral("PlayableMediaTypes"), QJsonArray{QStringLiteral("Video")}},
                    {QStringLiteral("SupportedCommands"), QJsonArray{}},
                    {QStringLiteral("SupportsMediaControl"), true},
                    {QStringLiteral("SupportsPersistentIdentifier"), true}});
    post(a, QStringLiteral("New"), {{QStringLiteral("GroupName"), groupName}});
    settle("new");
    const QString groupId = groupIdIn(listGroups(b, "after-new"));
    QVERIFY2(!groupId.isEmpty(), "the new group is not in B's list");

    // B joins first with no socket, then with a socket but no media control announced
    post(b, QStringLiteral("Join"), {{QStringLiteral("GroupId"), groupId}});
    settle("join-without-socket");
    listGroups(b, "join-without-socket");
    post(b, QStringLiteral("Leave"), {});
    settle("leave-without-socket");
    QVERIFY2(openSocketOf(b), "B's socket did not open");
    settle("b-socket-open");
    post(b, QStringLiteral("Join"), {{QStringLiteral("GroupId"), groupId}});
    settle("join");

    // B reports a 300 ms round trip before the queue, to see it in the Unpause lead
    post(b, QStringLiteral("Ping"), {{QStringLiteral("Ping"), 300}});
    post(a, QStringLiteral("SetNewQueue"),
         {{QStringLiteral("PlayingQueue"), QJsonArray{movie}},
          {QStringLiteral("PlayingItemPosition"), 0},
          {QStringLiteral("StartPositionTicks"), 0}});
    settle("new-queue");
    const QString entry = playingEntry(a);
    qInfo() << LIVE_FACT << "playing entry after SetNewQueue:" << entry << "B sees"
            << playingEntry(b);
    post(a, QStringLiteral("Ready"), readiness(0, false, entry));
    settle("ready-a", 1500);
    post(b, QStringLiteral("Ready"), readiness(0, false, entry));
    settle("ready-b");
    post(a, QStringLiteral("Unpause"), {});
    settle("unpause");
    post(b, QStringLiteral("Pause"), {});
    settle("pause");

    // A seek, then B ready 30 s off the asked position: does the server seek it again
    constexpr qint64 seekTicks = 60LL * 10'000'000;
    post(a, QStringLiteral("Seek"), {{QStringLiteral("PositionTicks"), seekTicks}});
    settle("seek");
    post(a, QStringLiteral("Ready"), readiness(seekTicks, false, entry));
    post(b, QStringLiteral("Ready"), readiness(seekTicks / 2, false, entry));
    settle("ready-off-position");
    post(a, QStringLiteral("Unpause"), {});
    settle("unpause-after-seek", 3000);

    post(b, QStringLiteral("Buffering"), readiness(seekTicks + 30'000'000, true, entry));
    settle("buffering");
    post(b, QStringLiteral("Ready"), readiness(seekTicks + 30'000'000, true, entry));
    settle("ready-after-buffering");
    post(b, QStringLiteral("SetIgnoreWait"), {{QStringLiteral("IgnoreWait"), true}});
    post(b, QStringLiteral("Buffering"), readiness(seekTicks + 60'000'000, true, entry));
    settle("buffering-ignoring-wait");
    post(b, QStringLiteral("SetIgnoreWait"), {{QStringLiteral("IgnoreWait"), false}});
    settle("ignore-wait-off", 1000);

    // The last entry ended: two NextItem of the same entry, as two members would send them
    post(a, QStringLiteral("NextItem"), {{QStringLiteral("PlaylistItemId"), entry}});
    post(b, QStringLiteral("NextItem"), {{QStringLiteral("PlaylistItemId"), entry}});
    settle("next-item-twice");

    // B's socket drops without a close handshake, then comes back: does the server re-join it
    b.socket->abort();
    settle("b-socket-dropped", 4000);
    listGroups(a, "b-socket-dropped");
    QVERIFY2(openSocketOf(b), "B's socket did not reopen");
    settle("b-socket-reopened", 6000);
    listGroups(a, "b-socket-reopened");

    post(b, QStringLiteral("Leave"), {});
    settle("leave");
    post(b, QStringLiteral("Leave"), {});
    settle("leave-twice");
    post(b, QStringLiteral("Pause"), {});
    settle("pause-outside-a-group");
    post(b, QStringLiteral("Join"),
         {{QStringLiteral("GroupId"), QUuid::createUuid().toString(QUuid::Id128)}});
    settle("join-missing-group");
    post(a, QStringLiteral("SetNewQueue"),
         {{QStringLiteral("PlayingQueue"), QJsonArray{QUuid::createUuid().toString(QUuid::Id128)}},
          {QStringLiteral("PlayingItemPosition"), 0},
          {QStringLiteral("StartPositionTicks"), 0}});
    settle("queue-of-a-missing-item");
    post(a, QStringLiteral("Leave"), {});
    settle("last-member-leaves");
    QVERIFY2(groupIdIn(listGroups(b, "after-everyone-left")).isEmpty(),
             "the group outlived its last member");
}

void TestJellyfinLive::syncPlayQueueProtocol()
{
    if (!m_live)
        QSKIP("no KINEFIN_LIVE_* environment, skipping the live checks");

    struct Member {
        QString name;
        Kinefin::JellyfinClient client;
        QWebSocket *socket = nullptr;
        QStringList frames;
        qsizetype printed = 0;
    };
    Member a;
    a.name = QStringLiteral("A");
    Member b;
    b.name = QStringLiteral("B");
    QVERIFY(authenticate(a.client));
    QVERIFY(authenticate(b.client));
    const auto cleanup = qScopeGuard([&] {
        for (Member *member : {&a, &b}) {
            postJson(member->client, QStringLiteral("/SyncPlay/Leave"), {});
            delete member->socket;
            member->client.disconnectFromServer();
        }
    });
    const auto openSocketOf = [](Member &member) {
        auto *socket =
            new QWebSocket(QStringLiteral("Kinefin live check"), QWebSocketProtocol::VersionLatest);
        member.socket = socket;
        const QString keepAlive = QStringLiteral("{\"MessageType\":\"KeepAlive\"}");
        QObject::connect(socket, &QWebSocket::textMessageReceived, socket,
                         [&member, socket, keepAlive](const QString &frame) {
                             if (frame.contains(QLatin1String("ForceKeepAlive")))
                                 socket->sendTextMessage(keepAlive);
                             else if (!frame.contains(QLatin1String("\"KeepAlive\""))
                                      && !frame.contains(QLatin1String("LibraryChanged")))
                                 member.frames.append(frame);
                         });
        auto *beat = new QTimer(socket);
        QObject::connect(beat, &QTimer::timeout, socket,
                         [socket, keepAlive] { socket->sendTextMessage(keepAlive); });
        beat->start(25000);
        QSignalSpy connected(socket, &QWebSocket::connected);
        socket->open(member.client.webSocketRequest());
        return connected.wait(15000);
    };
    const auto settle = [&](const char *step, int ms = 2000) {
        QTest::qWait(ms);
        for (Member *member : {&a, &b}) {
            for (; member->printed < member->frames.size(); ++member->printed)
                qInfo().noquote() << LIVE_FACT << step << member->name
                                  << "frame:" << member->frames.at(member->printed);
        }
    };
    const auto post = [this](Member &member, const QString &route, const QJsonObject &body) {
        QByteArray answer;
        const int status =
            postJson(member.client, QStringLiteral("/SyncPlay/") + route, body, &answer);
        qInfo().noquote() << LIVE_FACT << member.name << QStringLiteral("POST /SyncPlay/") + route
                          << QJsonDocument(body).toJson(QJsonDocument::Compact) << "->" << status
                          << QString::fromUtf8(answer.left(400));
        return status;
    };
    const auto readiness = [](qint64 ticks, bool playing, const QString &entry) {
        return QJsonObject{
            {QStringLiteral("When"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
            {QStringLiteral("PositionTicks"), ticks},
            {QStringLiteral("IsPlaying"), playing},
            {QStringLiteral("PlaylistItemId"), entry}};
    };
    // The entries and the playing one of the latest PlayQueue update A received
    const auto queueOf = [&a](QStringList *entries) {
        for (qsizetype i = a.frames.size() - 1; i >= 0; --i) {
            const QJsonObject update = QJsonDocument::fromJson(a.frames.at(i).toUtf8())
                                           .object()
                                           .value(QStringLiteral("Data"))
                                           .toObject();
            if (update.value(QStringLiteral("Type")).toString() != QLatin1String("PlayQueue"))
                continue;
            const QJsonObject queue = update.value(QStringLiteral("Data")).toObject();
            entries->clear();
            for (const QJsonValue &entry : queue.value(QStringLiteral("Playlist")).toArray())
                entries->append(
                    entry.toObject().value(QStringLiteral("PlaylistItemId")).toString());
            return queue.value(QStringLiteral("PlayingItemIndex")).toInt(-1);
        }
        return -1;
    };

    const QJsonArray movies =
        getJson(a.client,
                QStringLiteral("/Items?IncludeItemTypes=Movie&Recursive=true&SortBy=Name&Limit=3"))
            .value(QStringLiteral("Items"))
            .toArray();
    if (movies.size() < 3)
        QSKIP("fewer than three movies in the user's libraries");
    QStringList ids;
    for (const QJsonValue &movie : movies)
        ids.append(movie.toObject().value(QStringLiteral("Id")).toString());

    QVERIFY(openSocketOf(a));
    QVERIFY(openSocketOf(b));
    post(a, QStringLiteral("New"),
         {{QStringLiteral("GroupName"),
           QStringLiteral("Kinefin queue check %1").arg(deviceSequence)}});
    settle("new");
    const QJsonArray groups = getDocument(b.client, QStringLiteral("/SyncPlay/List")).array();
    QString groupId;
    for (const QJsonValue &group : groups) {
        if (group.toObject()
                .value(QStringLiteral("GroupName"))
                .toString()
                .startsWith(QStringLiteral("Kinefin queue check")))
            groupId = group.toObject().value(QStringLiteral("GroupId")).toString();
    }
    QVERIFY2(!groupId.isEmpty(), "the new group is not listed");
    post(b, QStringLiteral("Join"), {{QStringLiteral("GroupId"), groupId}});
    settle("join");

    post(a, QStringLiteral("SetNewQueue"),
         {{QStringLiteral("PlayingQueue"), QJsonArray::fromStringList(ids)},
          {QStringLiteral("PlayingItemPosition"), 0},
          {QStringLiteral("StartPositionTicks"), 0}});
    settle("new-queue");
    QStringList entries;
    int playing = queueOf(&entries);
    QVERIFY2(entries.size() == 3 && playing == 0, "the queue of three did not come");
    post(a, QStringLiteral("Ready"), readiness(0, false, entries.at(0)));
    post(b, QStringLiteral("Ready"), readiness(0, false, entries.at(0)));
    settle("ready");

    // The first entry ended on both: NextItem twice, as two members would send it
    post(a, QStringLiteral("NextItem"), {{QStringLiteral("PlaylistItemId"), entries.at(0)}});
    post(b, QStringLiteral("NextItem"), {{QStringLiteral("PlaylistItemId"), entries.at(0)}});
    settle("next-item-twice");
    playing = queueOf(&entries);
    qInfo() << LIVE_FACT << "playing index after NextItem:" << playing;
    if (playing >= 0) {
        post(a, QStringLiteral("Ready"), readiness(0, false, entries.at(playing)));
        post(b, QStringLiteral("Ready"), readiness(0, false, entries.at(playing)));
        settle("ready-after-next");
        post(a, QStringLiteral("PreviousItem"),
             {{QStringLiteral("PlaylistItemId"), entries.at(playing)}});
        settle("previous-item");
    }
    playing = queueOf(&entries);
    post(a, QStringLiteral("SetPlaylistItem"), {{QStringLiteral("PlaylistItemId"), entries.at(2)}});
    settle("set-playlist-item");

    post(a, QStringLiteral("Queue"),
         {{QStringLiteral("ItemIds"), QJsonArray{ids.at(0)}},
          {QStringLiteral("Mode"), QStringLiteral("Queue")}});
    settle("queue");
    post(a, QStringLiteral("Queue"),
         {{QStringLiteral("ItemIds"), QJsonArray{ids.at(1)}},
          {QStringLiteral("Mode"), QStringLiteral("QueueNext")}});
    settle("queue-next");
    playing = queueOf(&entries);
    post(a, QStringLiteral("MovePlaylistItem"),
         {{QStringLiteral("PlaylistItemId"), entries.last()}, {QStringLiteral("NewIndex"), 0}});
    settle("move-last-to-first");
    playing = queueOf(&entries);
    const QString notPlaying = entries.at(playing == 0 ? 1 : 0);
    post(a, QStringLiteral("RemoveFromPlaylist"),
         {{QStringLiteral("PlaylistItemIds"), QJsonArray{notPlaying}},
          {QStringLiteral("ClearPlaylist"), false},
          {QStringLiteral("ClearPlayingItem"), false}});
    settle("remove-an-entry");
    playing = queueOf(&entries);
    if (playing >= 0)
        post(a, QStringLiteral("RemoveFromPlaylist"),
             {{QStringLiteral("PlaylistItemIds"), QJsonArray{entries.at(playing)}},
              {QStringLiteral("ClearPlaylist"), false},
              {QStringLiteral("ClearPlayingItem"), false}});
    settle("remove-the-playing-entry");

    post(a, QStringLiteral("SetRepeatMode"),
         {{QStringLiteral("Mode"), QStringLiteral("RepeatAll")}});
    settle("repeat-all");
    post(a, QStringLiteral("SetShuffleMode"),
         {{QStringLiteral("Mode"), QStringLiteral("Shuffle")}});
    settle("shuffle");
    post(a, QStringLiteral("SetShuffleMode"), {{QStringLiteral("Mode"), QStringLiteral("Sorted")}});
    settle("sorted");
    playing = queueOf(&entries);
    if (playing >= 0) {
        post(a, QStringLiteral("SetPlaylistItem"),
             {{QStringLiteral("PlaylistItemId"), entries.last()}});
        settle("last-entry");
        playing = queueOf(&entries);
        post(a, QStringLiteral("NextItem"),
             {{QStringLiteral("PlaylistItemId"), entries.at(playing)}});
        settle("next-item-at-the-end-under-repeat-all");
    }
    post(a, QStringLiteral("RemoveFromPlaylist"),
         {{QStringLiteral("PlaylistItemIds"), QJsonArray()},
          {QStringLiteral("ClearPlaylist"), true},
          {QStringLiteral("ClearPlayingItem"), true}});
    settle("clear");
    post(b, QStringLiteral("Leave"), {});
    post(a, QStringLiteral("Leave"), {});
    settle("left");
}

QTEST_MAIN(TestJellyfinLive)
#include "test_jellyfin_live.moc"
