// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "FakeJellyfin.h"
#include "api/DeviceProfile.h"
#include "api/JellyfinClient.h"

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

class TestJellyfinClient : public QObject {
    Q_OBJECT

  private Q_SLOTS:
    void bootstrapLoginAndViews()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::serverReady);
        QSignalSpy loggedIn(&client, &Kinefin::JellyfinClient::loginSucceeded);
        QSignalSpy views(&client, &Kinefin::JellyfinClient::userViewsReady);
        QSignalSpy failures(&client, &Kinefin::JellyfinClient::requestFailed);
        QVERIFY(prepare(client, server));
        QTRY_COMPARE(ready.size(), 1);
        QCOMPARE(client.serverId(), QStringLiteral("9c076c52ba5340d59f6baf5b57cf0a5e"));
        QCOMPARE(client.serverUrl(), server.url(QStringLiteral("/jellyfin")));
        QVERIFY(client.authenticate(QStringLiteral("fixture-user"),
                                    QStringLiteral("fixture-password")));
        QTRY_COMPARE(loggedIn.size(), 1);
        QVERIFY(client.isAuthenticated());
        QVERIFY(client.fetchUserViews());
        QTRY_COMPARE(views.size(), 1);
        const QList<QVariant> result = views.takeFirst();
        QCOMPARE(result.at(0).value<QJsonArray>().size(), 3);
        QCOMPARE(result.at(1).toInt(), 3);
        QCOMPARE(server.viewRequests, 1);
        QVERIFY(!server.lastLoginAuth.contains("Token="));
        QVERIFY(server.lastViewsAuth.contains("Token="));
        QCOMPARE(failures.size(), 0);
        client.disconnectFromServer();
        QVERIFY(!client.isAuthenticated());
        QVERIFY(client.userId().isEmpty());
    }

    void rejectsUnsafeAddressesAndIdentityHeaders()
    {
        Kinefin::JellyfinClient client;
        QVERIFY(!client.connectToServer(QUrl(QStringLiteral("file:///etc/passwd"))));
        QVERIFY(!client.connectToServer(QUrl(QStringLiteral("http://user:pass@example.com"))));
        QVERIFY(!client.connectToServer(QUrl(QStringLiteral("https://example.com/?secret=123"))));
        QVERIFY(!client.setDeviceIdentity(QStringLiteral("bad\r\nInjected: x"),
                                          QStringLiteral("id"), QStringLiteral("0.0.1")));
        QVERIFY(!client.setDeviceIdentity(QStringLiteral("bad\""), QStringLiteral("id"),
                                          QStringLiteral("0.0.1")));
    }

    void followsSafeSameHostPublicRedirect()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.relocatePublicOnSameHost = true;
        Kinefin::JellyfinClient client;
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::serverReady);
        QVERIFY(prepare(client, server));
        QTRY_COMPARE(ready.size(), 1);
        QCOMPARE(client.serverUrl(), server.url(QStringLiteral("/jellyfin-proxy")));
    }

    void rejectsPublicRedirectToDifferentHost()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.redirectPublicOffHost = true;
        Kinefin::JellyfinClient client;
        QSignalSpy failures(&client, &Kinefin::JellyfinClient::requestFailed);
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::serverReady);
        QVERIFY(prepare(client, server));
        QTRY_COMPARE(failures.size(), 1);
        QCOMPARE(ready.size(), 0);
        QVERIFY(client.serverUrl().isEmpty());
    }

    void rejectsInvalidServerIdentity()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.invalidIdentity = true;
        Kinefin::JellyfinClient client;
        QSignalSpy failures(&client, &Kinefin::JellyfinClient::requestFailed);
        QVERIFY(prepare(client, server));
        QTRY_COMPARE(failures.size(), 1);
        QVERIFY(client.serverId().isEmpty());
    }

    void failedLoginDoesNotAuthenticate()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.denyLogin = true;
        Kinefin::JellyfinClient client;
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::serverReady);
        QSignalSpy failures(&client, &Kinefin::JellyfinClient::requestFailed);
        QVERIFY(prepare(client, server));
        QTRY_COMPARE(ready.size(), 1);
        QVERIFY(client.authenticate(QStringLiteral("fixture-user"), QStringLiteral("wrong")));
        QTRY_COMPARE(failures.size(), 1);
        QCOMPARE(failures.takeFirst().at(0).toInt(), 401);
        QVERIFY(!client.isAuthenticated());
        QVERIFY(!client.fetchUserViews());
    }

    void unauthorizedLibraryResponseInvalidatesSession()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.denyViews = true;
        Kinefin::JellyfinClient client;
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::serverReady);
        QSignalSpy loggedIn(&client, &Kinefin::JellyfinClient::loginSucceeded);
        QSignalSpy expired(&client, &Kinefin::JellyfinClient::sessionExpired);
        QVERIFY(prepare(client, server));
        QTRY_COMPARE(ready.size(), 1);
        QVERIFY(client.authenticate(QStringLiteral("fixture-user"),
                                    QStringLiteral("fixture-password")));
        QTRY_COMPARE(loggedIn.size(), 1);
        QVERIFY(client.fetchUserViews());
        QTRY_COMPARE(expired.size(), 1);
        QVERIFY(!client.isAuthenticated());
        QVERIFY(client.userId().isEmpty());
        QVERIFY(!client.fetchUserViews());
    }

    void authRequestNeverFollowsRedirect()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.redirectViewsOffHost = true;
        Kinefin::JellyfinClient client;
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::serverReady);
        QSignalSpy loggedIn(&client, &Kinefin::JellyfinClient::loginSucceeded);
        QSignalSpy failures(&client, &Kinefin::JellyfinClient::requestFailed);
        QVERIFY(prepare(client, server));
        QTRY_COMPARE(ready.size(), 1);
        QVERIFY(client.authenticate(QStringLiteral("fixture-user"),
                                    QStringLiteral("fixture-password")));
        QTRY_COMPARE(loggedIn.size(), 1);
        QVERIFY(client.fetchUserViews());
        QTRY_COMPARE(failures.size(), 1);
        QCOMPARE(server.viewRequests, 1);
        QVERIFY(client.isAuthenticated());
    }

    void quickConnectApprovalAuthorizesTheSameDevice()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::serverReady);
        QSignalSpy code(&client, &Kinefin::JellyfinClient::quickConnectCodeReady);
        QSignalSpy waiting(&client, &Kinefin::JellyfinClient::quickConnectAwaitingApproval);
        QSignalSpy loggedIn(&client, &Kinefin::JellyfinClient::loginSucceeded);
        QSignalSpy views(&client, &Kinefin::JellyfinClient::userViewsReady);
        QSignalSpy failures(&client, &Kinefin::JellyfinClient::requestFailed);
        QVERIFY(prepare(client, server));
        QTRY_COMPARE(ready.size(), 1);
        QVERIFY(client.initiateQuickConnect());
        QVERIFY(client.isQuickConnectPending());
        QTRY_COMPARE(code.size(), 1);
        QCOMPARE(code.constFirst().constFirst().toString(), QStringLiteral("AB12CD"));
        QVERIFY(!code.constFirst().constFirst().toString().contains(
            QStringLiteral("fixture-quick-secret")));
        QCOMPARE(server.quickInitiationRequests, 1);
        QVERIFY(!client.authenticate(QStringLiteral("another"), QStringLiteral("password")));
        QVERIFY(!client.fetchUserViews());
        QVERIFY(client.pollQuickConnect());
        QTRY_COMPARE(waiting.size(), 1);
        QCOMPARE(server.quickStatusRequests, 1);
        QCOMPARE(server.lastQuickPollSecret, QStringLiteral("fixture-quick-secret"));
        QVERIFY(server.lastQuickPollAuth.isEmpty());
        server.approveQuickConnect = true;
        QVERIFY(client.pollQuickConnect());
        QTRY_COMPARE(loggedIn.size(), 1);
        QVERIFY(client.isAuthenticated());
        QVERIFY(!client.isQuickConnectPending());
        QCOMPARE(server.quickExchangeRequests, 1);
        QCOMPARE(QJsonDocument::fromJson(server.lastQuickExchangeBody)
                     .object()
                     .value(QStringLiteral("Secret"))
                     .toString(),
                 QStringLiteral("fixture-quick-secret"));
        QVERIFY(server.lastQuickExchangeAuth.contains("DeviceId=\"stable-test-id\""));
        QVERIFY(!server.lastQuickExchangeAuth.contains("Token="));
        QVERIFY(!client.pollQuickConnect());
        QVERIFY(client.fetchUserViews());
        QTRY_COMPARE(views.size(), 1);
        QVERIFY(server.lastViewsAuth.contains("Token="));
        QCOMPARE(failures.size(), 0);
        client.disconnectFromServer();
        QVERIFY(!client.isAuthenticated());
    }

    void quickConnectDisabledCanFallbackToPassword()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.disableQuickConnect = true;
        Kinefin::JellyfinClient client;
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::serverReady);
        QSignalSpy failures(&client, &Kinefin::JellyfinClient::requestFailed);
        QSignalSpy loggedIn(&client, &Kinefin::JellyfinClient::loginSucceeded);
        QVERIFY(prepare(client, server));
        QTRY_COMPARE(ready.size(), 1);
        QVERIFY(client.initiateQuickConnect());
        QTRY_COMPARE(failures.size(), 1);
        QCOMPARE(failures.takeFirst().at(0).toInt(), 401);
        QVERIFY(!client.isQuickConnectPending());
        QVERIFY(client.authenticate(QStringLiteral("fixture-user"),
                                    QStringLiteral("fixture-password")));
        QTRY_COMPARE(loggedIn.size(), 1);
    }

    void malformedQuickConnectCodeFailsClosed()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.invalidQuickConnectCode = true;
        Kinefin::JellyfinClient client;
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::serverReady);
        QSignalSpy code(&client, &Kinefin::JellyfinClient::quickConnectCodeReady);
        QSignalSpy failures(&client, &Kinefin::JellyfinClient::requestFailed);
        QVERIFY(prepare(client, server));
        QTRY_COMPARE(ready.size(), 1);
        QVERIFY(client.initiateQuickConnect());
        QTRY_COMPARE(failures.size(), 1);
        QVERIFY(!client.isQuickConnectPending());
        QCOMPARE(code.size(), 0);
        QCOMPARE(server.quickStatusRequests, 0);
    }

    void mismatchedQuickConnectSecretCancelsPairing()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.mismatchQuickConnectSecret = true;
        Kinefin::JellyfinClient client;
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::serverReady);
        QSignalSpy code(&client, &Kinefin::JellyfinClient::quickConnectCodeReady);
        QSignalSpy failures(&client, &Kinefin::JellyfinClient::requestFailed);
        QVERIFY(prepare(client, server));
        QTRY_COMPARE(ready.size(), 1);
        QVERIFY(client.initiateQuickConnect());
        QTRY_COMPARE(code.size(), 1);
        QVERIFY(client.pollQuickConnect());
        QTRY_COMPARE(failures.size(), 1);
        QVERIFY(!client.isQuickConnectPending());
        QVERIFY(!client.isAuthenticated());
        QCOMPARE(server.quickExchangeRequests, 0);
    }

    void quickConnectExpiredStatusCancelsPairing()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.rejectQuickConnectStatus = true;
        Kinefin::JellyfinClient client;
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::serverReady);
        QSignalSpy code(&client, &Kinefin::JellyfinClient::quickConnectCodeReady);
        QSignalSpy failures(&client, &Kinefin::JellyfinClient::requestFailed);
        QVERIFY(prepare(client, server));
        QTRY_COMPARE(ready.size(), 1);
        QVERIFY(client.initiateQuickConnect());
        QTRY_COMPARE(code.size(), 1);
        QVERIFY(client.pollQuickConnect());
        QTRY_COMPARE(failures.size(), 1);
        QCOMPARE(failures.constFirst().at(0).toInt(), 404);
        QVERIFY(!client.isQuickConnectPending());
        QVERIFY(!client.isAuthenticated());
        QCOMPARE(server.quickExchangeRequests, 0);
    }

    void quickConnectExchange401DoesNotLogIn()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.approveQuickConnect = true;
        server.denyQuickConnectExchange = true;
        Kinefin::JellyfinClient client;
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::serverReady);
        QSignalSpy code(&client, &Kinefin::JellyfinClient::quickConnectCodeReady);
        QSignalSpy failures(&client, &Kinefin::JellyfinClient::requestFailed);
        QSignalSpy loggedIn(&client, &Kinefin::JellyfinClient::loginSucceeded);
        QVERIFY(prepare(client, server));
        QTRY_COMPARE(ready.size(), 1);
        QVERIFY(client.initiateQuickConnect());
        QTRY_COMPARE(code.size(), 1);
        QVERIFY(client.pollQuickConnect());
        QTRY_COMPARE(failures.size(), 1);
        QCOMPARE(failures.constFirst().at(0).toInt(), 401);
        QVERIFY(!client.isAuthenticated());
        QVERIFY(!client.isQuickConnectPending());
        QCOMPARE(loggedIn.size(), 0);
        QCOMPARE(server.quickExchangeRequests, 1);
    }

    void cancellingPendingQuickConnectSuppressesLateReplies()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.holdQuickConnectPoll = true;
        Kinefin::JellyfinClient client;
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::serverReady);
        QSignalSpy code(&client, &Kinefin::JellyfinClient::quickConnectCodeReady);
        QSignalSpy failures(&client, &Kinefin::JellyfinClient::requestFailed);
        QSignalSpy loggedIn(&client, &Kinefin::JellyfinClient::loginSucceeded);
        QVERIFY(prepare(client, server));
        QTRY_COMPARE(ready.size(), 1);
        QVERIFY(client.initiateQuickConnect());
        QTRY_COMPARE(code.size(), 1);
        QVERIFY(client.pollQuickConnect());
        QVERIFY(!client.pollQuickConnect()); // prevent overlapping polls
        QTRY_COMPARE(server.quickStatusRequests, 1);
        client.cancelQuickConnect();
        QVERIFY(!client.isQuickConnectPending());
        QVERIFY(!client.pollQuickConnect());
        QTest::qWait(40);
        QCOMPARE(loggedIn.size(), 0);
        QCOMPARE(failures.size(), 0);
        QVERIFY(client.initiateQuickConnect()); // reconnect is not required
        QTRY_COMPARE(code.size(), 2);
        client.disconnectFromServer();
        QVERIFY(!client.isAuthenticated());
        QVERIFY(!client.isQuickConnectPending());
    }

    void disconnectCancelsOutstandingRequests()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.holdViews = true;
        Kinefin::JellyfinClient client;
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::serverReady);
        QSignalSpy loggedIn(&client, &Kinefin::JellyfinClient::loginSucceeded);
        QSignalSpy views(&client, &Kinefin::JellyfinClient::userViewsReady);
        QVERIFY(prepare(client, server));
        QTRY_COMPARE(ready.size(), 1);
        QVERIFY(client.authenticate(QStringLiteral("fixture-user"),
                                    QStringLiteral("fixture-password")));
        QTRY_COMPARE(loggedIn.size(), 1);
        QVERIFY(client.fetchUserViews());
        QTRY_COMPARE(server.viewRequests, 1);
        client.disconnectFromServer();
        QTest::qWait(40);
        QCOMPARE(views.size(), 0);
        QVERIFY(!client.isAuthenticated());
    }

    void playbackInfoDirect()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::playbackInfoReady);
        QSignalSpy failed(&client, &Kinefin::JellyfinClient::playbackInfoFailed);
        QVERIFY(client.requestPlaybackInfo(kPlayItem, 1234567890, Kinefin::buildDeviceProfile({})));
        QTRY_COMPARE(ready.size(), 1);
        QCOMPARE(failed.size(), 0);

        // Everything is in the body: no query string, UserId, ticks, bitrate and the golden profile.
        QCOMPARE(server.lastPlaybackInfoRoute,
                 QByteArray("/jellyfin/Items/" + kPlayItem.toLatin1() + "/PlaybackInfo"));
        QVERIFY(server.lastPlaybackInfoContentType.startsWith("application/json"));
        QVERIFY(server.lastPlaybackInfoAuth.contains(kTokenField));
        const QJsonObject body = QJsonDocument::fromJson(server.lastPlaybackInfoBody).object();
        QCOMPARE(body.value(QStringLiteral("UserId")).toString(), client.userId());
        QCOMPARE(body.value(QStringLiteral("StartTimeTicks")).toVariant().toLongLong(), 1234567890);
        QCOMPARE(body.value(QStringLiteral("MaxStreamingBitrate")).toVariant().toLongLong(),
                 Kinefin::JellyfinClient::DefaultMaxStreamingBitrate);
        QCOMPARE(
            body.value(QStringLiteral("DeviceProfile")).toObject(),
            QJsonDocument::fromJson(fixture(QStringLiteral("device_profile_default"))).object());

        const auto source = ready.takeFirst().at(0).value<Kinefin::PlaybackSource>();
        QCOMPARE(source.method, Kinefin::PlaybackSource::Method::Direct);
        QCOMPARE(source.itemId, kPlayItem);
        QCOMPARE(source.url.path(), QStringLiteral("/jellyfin/Videos/%1/stream").arg(kPlayItem));
        QCOMPARE(source.url.host(), QStringLiteral("127.0.0.1"));
        QVERIFY(!source.url.toString().contains(QString::fromLatin1(kFixtureToken)));
        QVERIFY(client.streamAuthorization().contains(kTokenField));
    }

    void playbackInfoTranscode()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.playbackInfoFixture = QStringLiteral("playbackinfo_transcode");
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::playbackInfoReady);
        QVERIFY(client.requestPlaybackInfo(kPlayItem, 0, Kinefin::buildDeviceProfile({})));
        QTRY_COMPARE(ready.size(), 1);
        const auto source = ready.takeFirst().at(0).value<Kinefin::PlaybackSource>();
        QCOMPARE(source.method, Kinefin::PlaybackSource::Method::Transcode);
        // The server base path is kept and the ApiKey the fixture embeds is gone.
        QVERIFY(source.url.path().startsWith(QStringLiteral("/jellyfin/videos/")));
        QVERIFY(!source.url.toString().contains(QString::fromLatin1(kFixtureToken)));
        QVERIFY(!QUrlQuery(source.url).hasQueryItem(QStringLiteral("ApiKey")));
    }

    void playbackInfo401ExpiresSession()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.playbackInfoStatus = 401;
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy expired(&client, &Kinefin::JellyfinClient::sessionExpired);
        QSignalSpy failed(&client, &Kinefin::JellyfinClient::playbackInfoFailed);
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::playbackInfoReady);
        QVERIFY(client.requestPlaybackInfo(kPlayItem, 0, Kinefin::buildDeviceProfile({})));
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(expired.size(), 1);
        QCOMPARE(failed.takeFirst().at(1).toInt(), 401);
        QCOMPARE(ready.size(), 0);
        QVERIFY(!client.isAuthenticated());
        QVERIFY(client.streamAuthorization().isEmpty());
    }

    void playbackInfoFailuresKeepTheSession()
    {
        // 400 is what a 12.4 server answers for an unknown item (observed), 404 is the documented code.
        for (const int status : {400, 404, 500, 302}) {
            FakeJellyfin server;
            QVERIFY(server.listen());
            server.playbackInfoStatus = status == 302 ? 0 : status;
            server.redirectPlaybackInfo = status == 302;
            Kinefin::JellyfinClient client;
            QVERIFY(login(client, server));
            QSignalSpy failed(&client, &Kinefin::JellyfinClient::playbackInfoFailed);
            QSignalSpy expired(&client, &Kinefin::JellyfinClient::sessionExpired);
            QVERIFY(client.requestPlaybackInfo(kPlayItem, 0, Kinefin::buildDeviceProfile({})));
            QTRY_COMPARE(failed.size(), 1);
            const QList<QVariant> result = failed.takeFirst();
            QCOMPARE(result.at(0).toString(), kPlayItem);
            QCOMPARE(result.at(1).toInt(), status);
            QCOMPARE(result.at(2).toString(), QStringLiteral("Playback request failed"));
            QCOMPARE(expired.size(), 0);
            QVERIFY(client.isAuthenticated());
        }
    }

    void malformedPlaybackInfoFailsClosed()
    {
        const QByteArray oversized = paddedPlaybackInfo(1'100'000);
        const QList<QPair<QByteArray, QString>> cases{
            {"not json", QStringLiteral("Invalid playback response")},
            {"[]", QStringLiteral("Invalid playback response")},
            {oversized, QStringLiteral("Invalid playback response")},
            {"{\"MediaSources\":[]}", QStringLiteral("Invalid playback response")},
            {"{\"MediaSources\":[],\"PlaySessionId\":\"b6c7d8e9f0a1b2c3\"}",
             QStringLiteral("Invalid playback response")},
            {"{\"MediaSources\":[{\"Id\":\"a1\"}],\"PlaySessionId\":\"b6c7d8e9f0a1b2c3\"}",
             QStringLiteral("No playable media source")},
            {"{\"MediaSources\":[],\"PlaySessionId\":\"x\",\"ErrorCode\":\"NotAllowed\"}",
             QStringLiteral("Playback is not allowed")}};
        for (const auto &[body, reason] : cases) {
            FakeJellyfin server;
            QVERIFY(server.listen());
            server.playbackInfoRawBody = body;
            Kinefin::JellyfinClient client;
            QVERIFY(login(client, server));
            QSignalSpy failed(&client, &Kinefin::JellyfinClient::playbackInfoFailed);
            QSignalSpy ready(&client, &Kinefin::JellyfinClient::playbackInfoReady);
            QVERIFY(client.requestPlaybackInfo(kPlayItem, 0, Kinefin::buildDeviceProfile({})));
            QTRY_COMPARE(failed.size(), 1);
            QCOMPARE(failed.takeFirst().at(2).toString(), reason);
            QCOMPARE(ready.size(), 0);
            QVERIFY(client.isAuthenticated());
        }
    }

    void newPlaybackRequestSupersedesTheHeldOne()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.holdPlaybackInfo = true;
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::playbackInfoReady);
        QSignalSpy failed(&client, &Kinefin::JellyfinClient::playbackInfoFailed);
        QVERIFY(
            client.requestPlaybackInfo(QStringLiteral("aaaa"), 0, Kinefin::buildDeviceProfile({})));
        QTRY_COMPARE(server.playbackInfoRequests, 1);
        server.holdPlaybackInfo = false;
        QVERIFY(client.requestPlaybackInfo(kPlayItem, 0, Kinefin::buildDeviceProfile({})));
        QTRY_COMPARE(ready.size(), 1);
        QTest::qWait(200);
        QCOMPARE(ready.size(), 1);
        QCOMPARE(failed.size(), 0);
        QCOMPARE(ready.takeFirst().at(0).value<Kinefin::PlaybackSource>().itemId, kPlayItem);
    }

    void cancelAndDisconnectSuppressPlaybackReplies()
    {
        for (const bool disconnect : {false, true}) {
            FakeJellyfin server;
            QVERIFY(server.listen());
            server.holdPlaybackInfo = true;
            Kinefin::JellyfinClient client;
            QVERIFY(login(client, server));
            QSignalSpy ready(&client, &Kinefin::JellyfinClient::playbackInfoReady);
            QSignalSpy failed(&client, &Kinefin::JellyfinClient::playbackInfoFailed);
            QVERIFY(client.requestPlaybackInfo(kPlayItem, 0, Kinefin::buildDeviceProfile({})));
            QTRY_COMPARE(server.playbackInfoRequests, 1);
            if (disconnect) {
                client.disconnectFromServer();
                QVERIFY(client.streamAuthorization().isEmpty());
            } else {
                client.cancelPlaybackInfo();
                QVERIFY(client.isAuthenticated());
            }
            QTest::qWait(200);
            QCOMPARE(ready.size(), 0);
            QCOMPARE(failed.size(), 0);
        }
    }

    void playbackInfoResponseSizeCap()
    {
        for (const bool oversize : {false, true}) {
            FakeJellyfin server;
            QVERIFY(server.listen());
            server.playbackInfoRawBody = paddedPlaybackInfo(oversize ? 1'100'000 : 900'000);
            Kinefin::JellyfinClient client;
            QVERIFY(login(client, server));
            QSignalSpy ready(&client, &Kinefin::JellyfinClient::playbackInfoReady);
            QSignalSpy failed(&client, &Kinefin::JellyfinClient::playbackInfoFailed);
            QVERIFY(client.requestPlaybackInfo(kPlayItem, 0, Kinefin::buildDeviceProfile({})));
            QTRY_COMPARE(ready.size() + failed.size(), 1);
            QCOMPARE(ready.size(), oversize ? 0 : 1);
        }
    }

    // A 401 on another call ends a held negotiation with sessionExpired and nothing else.
    void sessionLostElsewhereEndsPendingPlaybackInfoQuietly()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.holdPlaybackInfo = true;
        server.denyViews = true;
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy expired(&client, &Kinefin::JellyfinClient::sessionExpired);
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::playbackInfoReady);
        QSignalSpy failed(&client, &Kinefin::JellyfinClient::playbackInfoFailed);
        QVERIFY(client.requestPlaybackInfo(kPlayItem, 0, Kinefin::buildDeviceProfile({})));
        QTRY_COMPARE(server.playbackInfoRequests, 1);
        QVERIFY(client.fetchUserViews());
        QTRY_COMPARE(expired.size(), 1);
        QTest::qWait(200);
        QCOMPARE(ready.size(), 0);
        QCOMPARE(failed.size(), 0);
        QVERIFY(client.streamAuthorization().isEmpty());
    }

    void playbackInfoMayBeRequestedFromAFailureSlot()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.playbackInfoStatus = 500;
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::playbackInfoReady);
        QSignalSpy failed(&client, &Kinefin::JellyfinClient::playbackInfoFailed);
        int retries = 0;
        QObject::connect(&client, &Kinefin::JellyfinClient::playbackInfoFailed, &client, [&] {
            if (retries++ > 0)
                return;
            server.playbackInfoStatus = 0;
            QVERIFY(client.requestPlaybackInfo(kPlayItem, 0, Kinefin::buildDeviceProfile({})));
        });
        QVERIFY(client.requestPlaybackInfo(kPlayItem, 0, Kinefin::buildDeviceProfile({})));
        QTRY_COMPARE(ready.size(), 1);
        QCOMPARE(failed.size(), 1);
    }

    void capabilitiesAreSentAfterLogin()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QTRY_COMPARE(server.capabilitiesRequests, 1);
        QCOMPARE(QJsonDocument::fromJson(server.lastCapabilitiesBody).object(),
                 QJsonDocument::fromJson(fixture(QStringLiteral("client_capabilities_request")))
                     .object());
        QVERIFY(server.lastCapabilitiesAuth.contains(kTokenField));
        // Sent again only on request, never retried by itself.
        QVERIFY(client.reportCapabilities());
        QTRY_COMPARE(server.capabilitiesRequests, 2);
    }

    void reportsGoOutInOrderWithTheHeaderOnly()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy settled(&client, &Kinefin::JellyfinClient::playbackReportsSettled);
        const QStringList goldens{QStringLiteral("playback_start_request"),
                                  QStringLiteral("playback_progress_request"),
                                  QStringLiteral("playback_stopped_request")};
        const QByteArrayList routes{"/jellyfin/Sessions/Playing",
                                    "/jellyfin/Sessions/Playing/Progress",
                                    "/jellyfin/Sessions/Playing/Stopped"};
        for (int i = 0; i < 3; ++i) {
            Kinefin::PlaybackReport report;
            report.kind = i == 0   ? Kinefin::PlaybackReport::Kind::Start
                          : i == 1 ? Kinefin::PlaybackReport::Kind::Progress
                                   : Kinefin::PlaybackReport::Kind::Stopped;
            report.itemId = kPlayItem;
            report.mediaSourceId = QStringLiteral("c1b2a3d4e5f6071829304a5b6c7d8e9f");
            report.playSessionId = QStringLiteral("b6c7d8e9f0a1b2c3");
            report.positionTicks = i == 0 ? 600000000 : i == 1 ? 123456789 : 100540000000;
            report.isPaused = i == 1;
            report.method = i == 1 ? Kinefin::PlaybackSource::Method::Transcode
                                   : Kinefin::PlaybackSource::Method::Direct;
            QVERIFY(client.reportPlayback(report));
            QTRY_COMPARE(server.reportLog.size(), i + 1);
            QTRY_COMPARE(settled.size(), i + 1);
        }
        for (int i = 0; i < 3; ++i) {
            const auto &entry = server.reportLog.at(i);
            QCOMPARE(entry.route, routes.at(i));
            QVERIFY(entry.contentType.startsWith("application/json"));
            QVERIFY(entry.auth.contains(kTokenField));
            QVERIFY(!entry.body.contains(kFixtureToken));
            QCOMPARE(QJsonDocument::fromJson(entry.body).object(),
                     QJsonDocument::fromJson(fixture(goldens.at(i))).object());
        }
        QVERIFY(!client.hasPendingPlaybackReports());
    }

    void progressCoalescesBehindTheReportInFlight()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.holdReports = true;
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy settled(&client, &Kinefin::JellyfinClient::playbackReportsSettled);
        const auto send = [&](Kinefin::PlaybackReport::Kind kind, qint64 ticks,
                              const QString &session) {
            Kinefin::PlaybackReport report;
            report.kind = kind;
            report.itemId = kPlayItem;
            report.mediaSourceId = QStringLiteral("aaaa");
            report.playSessionId = session;
            report.positionTicks = ticks;
            return client.reportPlayback(report);
        };
        QVERIFY(send(Kinefin::PlaybackReport::Kind::Start, 0, QStringLiteral("s1")));
        QTRY_COMPARE(server.reportLog.size(), 1);
        for (qint64 ticks : {1, 2, 3})
            QVERIFY(send(Kinefin::PlaybackReport::Kind::Progress, ticks, QStringLiteral("s1")));
        // One request at a time: nothing else left while the Start is unanswered.
        QTest::qWait(100);
        QCOMPARE(server.reportLog.size(), 1);
        server.holdReports = false;
        server.releaseHeld();
        QTRY_COMPARE(settled.size(), 1);
        QCOMPARE(server.reportLog.size(), 2);
        QCOMPARE(QJsonDocument::fromJson(server.reportLog.at(1).body)
                     .object()
                     .value(QStringLiteral("PositionTicks"))
                     .toVariant()
                     .toLongLong(),
                 3);
    }

    void stoppedDropsQueuedProgressAndKeepsOrderAcrossSessions()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.holdReports = true;
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy settled(&client, &Kinefin::JellyfinClient::playbackReportsSettled);
        const auto send = [&](Kinefin::PlaybackReport::Kind kind, const QString &session) {
            Kinefin::PlaybackReport report;
            report.kind = kind;
            report.itemId = kPlayItem;
            report.mediaSourceId = QStringLiteral("aaaa");
            report.playSessionId = session;
            return client.reportPlayback(report);
        };
        QVERIFY(send(Kinefin::PlaybackReport::Kind::Start, QStringLiteral("s1")));
        QTRY_COMPARE(server.reportLog.size(), 1);
        QVERIFY(send(Kinefin::PlaybackReport::Kind::Progress, QStringLiteral("s1")));
        QVERIFY(send(Kinefin::PlaybackReport::Kind::Stopped, QStringLiteral("s1")));
        QVERIFY(send(Kinefin::PlaybackReport::Kind::Start, QStringLiteral("s2")));
        server.holdReports = false;
        server.releaseHeld();
        QTRY_COMPARE(settled.size(), 1);
        // The old session's Stopped precedes the new session's Start; the queued Progress is gone.
        QStringList sequence;
        for (const auto &entry : server.reportLog) {
            sequence << QString::fromLatin1(entry.route.mid(entry.route.lastIndexOf('/') + 1))
                            + QLatin1Char(':')
                            + QJsonDocument::fromJson(entry.body)
                                  .object()
                                  .value(QStringLiteral("PlaySessionId"))
                                  .toString();
        }
        QCOMPARE(sequence, (QStringList{QStringLiteral("Playing:s1"), QStringLiteral("Stopped:s1"),
                                        QStringLiteral("Playing:s2")}));
    }

    void onlyStoppedIsRetriedAndOnlyOnce()
    {
        const auto run = [](Kinefin::PlaybackReport::Kind kind, int status, bool onlyFirst) {
            FakeJellyfin server;
            if (!server.listen())
                return -1;
            server.reportStatus = onlyFirst ? 204 : status;
            server.firstStoppedStatus = onlyFirst ? status : 0;
            Kinefin::JellyfinClient client;
            client.setPlaybackReportRetryDelay(std::chrono::milliseconds(30));
            if (!login(client, server))
                return -1;
            QSignalSpy settled(&client, &Kinefin::JellyfinClient::playbackReportsSettled);
            Kinefin::PlaybackReport report;
            report.kind = kind;
            report.itemId = kPlayItem;
            report.mediaSourceId = QStringLiteral("aaaa");
            report.playSessionId = QStringLiteral("s1");
            if (!client.reportPlayback(report)
                || !QTest::qWaitFor([&] { return settled.size() == 1; }, 5000))
                return -2;
            QTest::qWait(150); // a stray second retry would show up here
            return int(server.reportLog.size());
        };
        using Kind = Kinefin::PlaybackReport::Kind;
        QCOMPARE(run(Kind::Stopped, 503, true), 2);  // 503 once: retried and delivered
        QCOMPARE(run(Kind::Stopped, 503, false), 2); // 503 forever: one retry, then dropped
        QCOMPARE(run(Kind::Stopped, 502, true), 2);
        QCOMPARE(run(Kind::Stopped, 504, true), 2);
        QCOMPARE(run(Kind::Stopped, 400, false), 1); // not a transient status
        QCOMPARE(run(Kind::Stopped, 500, false), 1);
        QCOMPARE(run(Kind::Start, 503, false), 1);    // Start would count a second play
        QCOMPARE(run(Kind::Progress, 503, false), 1); // the next Progress replaces it
    }

    void reportRedirectIsNotFollowedOrRetried()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.redirectReports = true;
        Kinefin::JellyfinClient client;
        client.setPlaybackReportRetryDelay(std::chrono::milliseconds(30));
        QVERIFY(login(client, server));
        QSignalSpy settled(&client, &Kinefin::JellyfinClient::playbackReportsSettled);
        Kinefin::PlaybackReport report;
        report.kind = Kinefin::PlaybackReport::Kind::Stopped;
        report.itemId = kPlayItem;
        report.mediaSourceId = QStringLiteral("aaaa");
        report.playSessionId = QStringLiteral("s1");
        QVERIFY(client.reportPlayback(report));
        QTRY_COMPARE(settled.size(), 1);
        QTest::qWait(150);
        QCOMPARE(server.reportLog.size(), 1);
        QVERIFY(client.isAuthenticated());
    }

    void report401ExpiresTheSessionAndDropsTheQueue()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.holdReports = true;
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy expired(&client, &Kinefin::JellyfinClient::sessionExpired);
        QSignalSpy settled(&client, &Kinefin::JellyfinClient::playbackReportsSettled);
        const auto send = [&](Kinefin::PlaybackReport::Kind kind) {
            Kinefin::PlaybackReport report;
            report.kind = kind;
            report.itemId = kPlayItem;
            report.mediaSourceId = QStringLiteral("aaaa");
            report.playSessionId = QStringLiteral("s1");
            return client.reportPlayback(report);
        };
        QVERIFY(send(Kinefin::PlaybackReport::Kind::Start));
        QTRY_COMPARE(server.reportLog.size(), 1);
        QVERIFY(send(Kinefin::PlaybackReport::Kind::Progress));
        server.reportStatus = 401;
        server.holdReports = false;
        server.releaseHeld();
        QTRY_COMPARE(expired.size(), 1);
        QTest::qWait(100);
        QCOMPARE(server.reportLog.size(), 1); // the queued Progress never went out
        QVERIFY(!client.hasPendingPlaybackReports());
        QCOMPARE(settled.size(), 0);
        QVERIFY(!client.isAuthenticated());
    }

    void disconnectDropsPendingReportsQuietly()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        server.holdReports = true;
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy settled(&client, &Kinefin::JellyfinClient::playbackReportsSettled);
        Kinefin::PlaybackReport report;
        report.kind = Kinefin::PlaybackReport::Kind::Start;
        report.itemId = kPlayItem;
        report.mediaSourceId = QStringLiteral("aaaa");
        report.playSessionId = QStringLiteral("s1");
        QVERIFY(client.reportPlayback(report));
        QTRY_COMPARE(server.reportLog.size(), 1);
        report.kind = Kinefin::PlaybackReport::Kind::Progress;
        QVERIFY(client.reportPlayback(report));
        QVERIFY(client.hasPendingPlaybackReports());
        client.disconnectFromServer();
        QVERIFY(!client.hasPendingPlaybackReports());
        server.holdReports = false;
        server.releaseHeld();
        QTest::qWait(150);
        QCOMPARE(settled.size(), 0);
        QCOMPARE(server.reportLog.size(), 1);
    }

    void reportsAreRefusedWithoutARequest()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        Kinefin::PlaybackReport report;
        report.itemId = kPlayItem;
        report.mediaSourceId = QStringLiteral("aaaa");
        report.playSessionId = QStringLiteral("s1");
        QVERIFY(!client.reportPlayback(report)); // not authenticated
        QVERIFY(!client.reportCapabilities());
        QVERIFY(login(client, server));
        report.positionTicks = -1;
        QVERIFY(!client.reportPlayback(report));
        report.positionTicks = 0;
        report.itemId = QStringLiteral("a/b");
        QVERIFY(!client.reportPlayback(report));
        QTest::qWait(100);
        QCOMPARE(server.reportLog.size(), 0);
    }

    void playbackInfoRejectsBadInputWithoutARequest()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        const QJsonObject profile = Kinefin::buildDeviceProfile({});
        // Not authenticated yet.
        QVERIFY(!client.requestPlaybackInfo(kPlayItem, 0, profile));
        QVERIFY(client.streamAuthorization().isEmpty());
        QVERIFY(login(client, server));
        QVERIFY(!client.requestPlaybackInfo(QStringLiteral("a/b"), 0, profile));
        QVERIFY(!client.requestPlaybackInfo(QString(), 0, profile));
        QVERIFY(!client.requestPlaybackInfo(kPlayItem, -1, profile));
        QVERIFY(!client.requestPlaybackInfo(kPlayItem, 0, {}));
        QVERIFY(!client.requestPlaybackInfo(kPlayItem, 0, profile, 0));
        QTest::qWait(100);
        QCOMPARE(server.playbackInfoRequests, 0);
    }

    void webSocketRequestDerivation()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(prepare(client, server));
        // Without a session there is no socket request at all, not even a URL to leak.
        QVERIFY(client.webSocketRequest().url().isEmpty());
        QVERIFY(login(client, server));
        const QNetworkRequest socketRequest = client.webSocketRequest();
        // The loopback transport keeps plain ws for the /socket path of the base; the token travels only in the header.
        const QUrl expected = QUrl(QStringLiteral("ws://127.0.0.1:%1%2")
                                       .arg(server.url().port())
                                       .arg(QStringLiteral("/jellyfin/socket")));
        QCOMPARE(socketRequest.url(), expected);
        QVERIFY(socketRequest.url().query().isEmpty());
        QVERIFY(socketRequest.rawHeader(QByteArray("Authorization")).contains(kTokenField));
        client.disconnectFromServer();
        QVERIFY(client.webSocketRequest().url().isEmpty());
        QVERIFY(!client.webSocketRequest().hasRawHeader(QByteArray("Authorization")));
    }

    void verifySession401Expires()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy expired(&client, &Kinefin::JellyfinClient::sessionExpired);
        QSignalSpy cleared(&client, &Kinefin::JellyfinClient::credentialsCleared);
        // Anything but a 401 stays silent.
        QVERIFY(client.verifySession());
        QTRY_COMPARE(server.usersMeRequests, 1);
        QVERIFY(server.lastUsersMeAuth.contains(kTokenField));
        QVERIFY(client.isAuthenticated());
        QCOMPARE(expired.count(), 0);
        // The 401 expires the session and forgets the token.
        server.denyUsersMe = true;
        QVERIFY(client.verifySession());
        QTRY_COMPARE(expired.count(), 1);
        QCOMPARE(cleared.count(), 1);
        QVERIFY(!client.isAuthenticated());
        QVERIFY(client.userId().isEmpty());
        // Without a session the re-check cannot even be sent.
        QVERIFY(!client.verifySession());
        QVERIFY(client.webSocketRequest().url().isEmpty());
    }

    void credentialsClearedOnEveryTokenLoss()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy cleared(&client, &Kinefin::JellyfinClient::credentialsCleared);
        // Loss 1: a 401 on an authenticated route.
        server.denyViews = true;
        QVERIFY(client.fetchUserViews());
        QTRY_COMPARE(cleared.count(), 1);
        // Loss 2: an explicit logout.
        QVERIFY(login(client, server));
        QCOMPARE(cleared.count(), 1); // the fresh login had nothing left to forget
        client.disconnectFromServer();
        QCOMPARE(cleared.count(), 2);
        // Loss 3: destruction while a token is still held.
        bool destroyedClear = false;
        connect(&client, &Kinefin::JellyfinClient::credentialsCleared, this,
                [&destroyedClear] { destroyedClear = true; });
        {
            Kinefin::JellyfinClient dying;
            connect(&dying, &Kinefin::JellyfinClient::credentialsCleared, this,
                    [&destroyedClear] { destroyedClear = true; });
            QVERIFY(login(dying, server));
        }
        QVERIFY(destroyedClear);
    }
};

QTEST_GUILESS_MAIN(TestJellyfinClient)
#include "test_jellyfin_client.moc"
