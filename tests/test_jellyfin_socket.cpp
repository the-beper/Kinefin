// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "FakeJellyfin.h"
#include "api/JellyfinClient.h"
#include "api/JellyfinSocketClient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QtTest>

namespace {

const QString kFixtureUserId = QStringLiteral("d5ed5d4e2e5447b58e9f2b6f7bd5c96a");

// A single sink for every generated log line: the redaction test reads it flow by flow, the rest of the suite runs with capture off (lines still forwarded to the default handler)
QStringList *capturedMessages = nullptr;
QtMessageHandler previousHandler = nullptr;
void captureHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    if (capturedMessages)
        capturedMessages->append(message);
    if (previousHandler)
        previousHandler(type, context, message);
}

struct LogCapture {
    LogCapture() { capturedMessages = &messages; }
    ~LogCapture() { capturedMessages = nullptr; }
    bool leakFree(const QByteArray &forbidden) const
    {
        for (const QString &line : messages)
            if (line.toUtf8().contains(forbidden))
                return false;
        return true;
    }
    QStringList messages;
};

// Wraps an array or a raw value into a server frame: an object envelope plus MessageType
QString enveloped(const QString &type, const QJsonValue &data)
{
    const QJsonObject envelope{{QStringLiteral("MessageType"), type},
                               {QStringLiteral("Data"), data}};
    return QString::fromUtf8(QJsonDocument(envelope).toJson(QJsonDocument::Compact));
}

} // namespace

class TestJellyfinSocketClient : public QObject {
    Q_OBJECT

  private Q_SLOTS:
    void initTestCase() { previousHandler = qInstallMessageHandler(captureHandler); }

    void cleanupTestCase()
    {
        qInstallMessageHandler(previousHandler);
        previousHandler = nullptr;
    }

    void reconnectDelayBounds()
    {
        using Kinefin::JellyfinSocketClient;
        QCOMPARE(JellyfinSocketClient::reconnectDelay(0, 1000, 60000, 0.0), qint64(500));
        QCOMPARE(JellyfinSocketClient::reconnectDelay(0, 1000, 60000, 1.0), qint64(1000));
        QCOMPARE(JellyfinSocketClient::reconnectDelay(0, 1000, 60000, 0.5), qint64(750));
        QCOMPARE(JellyfinSocketClient::reconnectDelay(1, 1000, 60000, 0.0), qint64(1000));
        QCOMPARE(JellyfinSocketClient::reconnectDelay(1, 1000, 60000, 1.0), qint64(2000));
        QCOMPARE(JellyfinSocketClient::reconnectDelay(5, 1000, 60000, 0.0), qint64(16000));
        // The cap holds the ceiling: 1000·2ⁿ passes 60000 and stays there for every larger attempt
        QCOMPARE(JellyfinSocketClient::reconnectDelay(6, 1000, 60000, 0.0), qint64(30000));
        QCOMPARE(JellyfinSocketClient::reconnectDelay(6, 1000, 60000, 1.0), qint64(60000));
        QCOMPARE(JellyfinSocketClient::reconnectDelay(6, 1000, 60000, 0.5), qint64(45000));
        QCOMPARE(JellyfinSocketClient::reconnectDelay(1000, 1000, 60000, 0.0), qint64(30000));
        QCOMPARE(JellyfinSocketClient::reconnectDelay(-3, 1000, 60000, 0.0), qint64(500));
        // A ceiling beneath the base clamps both draws
        QCOMPARE(JellyfinSocketClient::reconnectDelay(0, 10, 5, 0.0), qint64(2));
        QCOMPARE(JellyfinSocketClient::reconnectDelay(0, 10, 5, 1.0), qint64(5));
    }

    void handshakeUsesHeaderOnly()
    {
        ConnectedSocket flow;
        QVERIFY(flow.connectAndAuthenticate());
        QVERIFY(flow.waitConnection());
        QCOMPARE(flow.server.upgradeAttempts, 1);
        QCOMPARE(flow.server.lastSocketRoute, QStringLiteral("/jellyfin/socket"));
        QCOMPARE(flow.server.lastSocketQuery, QString());
        QVERIFY(!flow.server.lastSocketRoute.contains(QLatin1Char('?')));
        const QByteArray expectation =
            QByteArray("MediaBrowser Client=\"Kinefin\", Device=\"Kinefin test\", "
                       "DeviceId=\"stable-test-id\", Version=\"0.0.1\", Token=\"")
            + kFixtureToken + "\"";
        QCOMPARE(flow.server.lastSocketAuth, expectation);
        QVERIFY(!flow.server.lastSocketAuth.contains(QByteArray("api_key")));
        QCOMPARE(flow.socket.state(), Kinefin::JellyfinSocketClient::State::Authenticated);
    }

    void keepAliveCycle()
    {
        ConnectedSocket flow;
        QVERIFY(flow.connectAndAuthenticate());
        FakeSocketConnection *connection = flow.waitConnection();
        QVERIFY(connection);
        // The server opens the heartbeat with its observed timeout; the answer goes out at once
        connection->sendText(QString::fromUtf8(fixture(QStringLiteral("ws_force_keepalive"))));
        QTRY_COMPARE(connection->receivedTexts.count(), 1);
        QCOMPARE(connection->receivedTexts.first(),
                 QStringLiteral("{\"MessageType\":\"KeepAlive\"}"));
        // Then the schedule continues at min(Data/2, 30 s); the override shortens it for the test
        flow.socket.setTestTimings(0, 0, 50, 0, 0);
        connection->sendText(QString::fromUtf8(fixture(QStringLiteral("ws_force_keepalive"))));
        QTRY_VERIFY(connection->receivedTexts.count() >= 3);
        for (const QString &text : connection->receivedTexts)
            QCOMPARE(text, QStringLiteral("{\"MessageType\":\"KeepAlive\"}"));
    }

    void deadPeerReconnects()
    {
        ConnectedSocket flow(0, 60);
        QVERIFY(flow.connectAndAuthenticate());
        QVERIFY(flow.waitConnection());
        QCOMPARE(flow.server.upgradeAttempts, 1);
        // Silence beyond the peer timer, then the same identity returns without a new credential
        QTRY_VERIFY(flow.server.upgradeAttempts >= 2);
        QVERIFY(flow.waitState(Kinefin::JellyfinSocketClient::State::Authenticated, 3000));
        QVERIFY(flow.server.lastConnection() && flow.server.lastConnection()->alive());
    }

    void connectTimeout()
    {
        ConnectedSocket flow(80);
        flow.server.holdSocketUpgrade = true;
        QVERIFY(flow.connectAndAuthenticate());
        // The handshake never completes: the timer kills the attempt, the session is re-checked and the ladder advances instead of stopping
        QTRY_VERIFY(flow.server.upgradeAttempts >= 2);
        QVERIFY(flow.server.usersMeRequests >= 1);
        QCOMPARE(flow.server.lastSocketQuery, QString());
        QVERIFY(flow.socket.state() == Kinefin::JellyfinSocketClient::State::Connecting
                || flow.socket.state() == Kinefin::JellyfinSocketClient::State::Reconnecting);
    }

    void routesLibraryChanged()
    {
        ConnectedSocket flow;
        QVERIFY(flow.connectAndAuthenticate());
        FakeSocketConnection *connection = flow.waitConnection();
        QVERIFY(connection);
        QSignalSpy library(&flow.socket, &Kinefin::JellyfinSocketClient::libraryChanged);
        connection->sendText(QString::fromUtf8(fixture(QStringLiteral("ws_library_changed"))));
        QTRY_COMPARE(library.count(), 1);
        // The payload characters are never parsed by the socket: it is a wake-up only
        QTest::qWait(50);
        QCOMPARE(library.count(), 1);
    }

    void routesUserDataChanged()
    {
        ConnectedSocket flow;
        QVERIFY(flow.connectAndAuthenticate());
        FakeSocketConnection *connection = flow.waitConnection();
        QVERIFY(connection);
        QSignalSpy data(&flow.socket, &Kinefin::JellyfinSocketClient::userDataChanged);
        connection->sendText(QString::fromUtf8(fixture(QStringLiteral("ws_user_data_changed"))));
        QTRY_COMPARE(data.count(), 1);
        const QStringList ids = data.first().first().value<QStringList>();
        QCOMPARE(ids.count(), 2);
        QVERIFY(ids.contains(QStringLiteral("c7dd3b1ac7dd3b1a9d42f2ab74f8a1b9")));
        QVERIFY(ids.contains(QStringLiteral("f9e6e5c39de743d9a19b8cbdf63b1dc1")));
        // Wrong user, an invalid id and a list beyond the cap are all dropped whole
        const QJsonObject wrongUser{
            {QStringLiteral("UserId"), QStringLiteral("00000000000000000000000000000001")},
            {QStringLiteral("ItemsIdList"), QJsonArray{QJsonValue(kPlayItem)}}};
        connection->sendText(enveloped(QStringLiteral("UserDataChanged"), wrongUser));
        const QJsonObject badId{{QStringLiteral("UserId"), kFixtureUserId},
                                {QStringLiteral("ItemsIdList"),
                                 QJsonArray{QJsonValue(QStringLiteral("../etc/passwd"))}}};
        connection->sendText(enveloped(QStringLiteral("UserDataChanged"), badId));
        QJsonObject oversizeList{{QStringLiteral("UserId"), kFixtureUserId}};
        QJsonArray tooMany;
        for (int i = 0; i < 10001; ++i)
            tooMany.append(QJsonValue(kPlayItem));
        oversizeList.insert(QStringLiteral("ItemsIdList"), tooMany);
        connection->sendText(enveloped(QStringLiteral("UserDataChanged"), oversizeList));
        QTest::qWait(100);
        QCOMPARE(data.count(), 1);
        // The connection survived the drops: the next valid frame still routes
        connection->sendText(QString::fromUtf8(fixture(QStringLiteral("ws_user_data_changed"))));
        QTRY_COMPARE(data.count(), 2);
        QSignalSpy playstate(&flow.socket, &Kinefin::JellyfinSocketClient::playstateCommand);
        connection->sendText(
            QStringLiteral("{\"MessageType\":\"Playstate\",\"Data\":{\"Command\":\"Seek\"}}"));
        QTRY_COMPARE(playstate.count(), 1);
    }

    void routesPlaystate()
    {
        ConnectedSocket flow;
        QVERIFY(flow.connectAndAuthenticate());
        FakeSocketConnection *connection = flow.waitConnection();
        QVERIFY(connection);
        QSignalSpy playstate(&flow.socket, &Kinefin::JellyfinSocketClient::playstateCommand);
        connection->sendText(QString::fromUtf8(fixture(QStringLiteral("ws_playstate"))));
        QTRY_COMPARE(playstate.count(), 1);
        const QList<QVariant> arguments = playstate.takeFirst();
        QCOMPARE(arguments.first().toString(), QStringLiteral("Seek"));
        QCOMPARE(arguments.last().toLongLong(), qint64(12345));
        // Without ticks the sentinel is -1: still a valid command, unlike the negative, fractional and non-letter ones
        const QJsonObject unticked{{QStringLiteral("Command"), QStringLiteral("Pause")}};
        connection->sendText(enveloped(QStringLiteral("Playstate"), unticked));
        QTRY_COMPARE(playstate.count(), 1);
        const QList<QVariant> untickedArguments = playstate.takeFirst();
        QCOMPARE(untickedArguments.first().toString(), QStringLiteral("Pause"));
        QCOMPARE(untickedArguments.last().toLongLong(), qint64(-1));
        const QJsonObject negative{{QStringLiteral("Command"), QStringLiteral("Seek")},
                                   {QStringLiteral("SeekPositionTicks"), QJsonValue(-5)}};
        connection->sendText(enveloped(QStringLiteral("Playstate"), negative));
        const QJsonObject fractional{{QStringLiteral("Command"), QStringLiteral("Seek")},
                                     {QStringLiteral("SeekPositionTicks"), QJsonValue(1.5)}};
        connection->sendText(enveloped(QStringLiteral("Playstate"), fractional));
        const QJsonObject wordy{{QStringLiteral("Command"), QStringLiteral("has Zendigits1234")}};
        connection->sendText(enveloped(QStringLiteral("Playstate"), wordy));
        QTest::qWait(100);
        QCOMPARE(playstate.count(), 0);
    }

    void routesSyncPlayFrames()
    {
        using Kinefin::SyncPlay::Command;
        using Kinefin::SyncPlay::GroupUpdate;
        ConnectedSocket flow;
        QVERIFY(flow.connectAndAuthenticate());
        FakeSocketConnection *connection = flow.waitConnection();
        QVERIFY(connection);
        QSignalSpy commands(&flow.socket, &Kinefin::JellyfinSocketClient::syncPlayCommand);
        QSignalSpy updates(&flow.socket, &Kinefin::JellyfinSocketClient::syncPlayGroupUpdate);
        // What a join brings, in the observed order
        connection->sendText(
            QString::fromUtf8(fixture(QStringLiteral("ws_syncplay_group_joined"))));
        connection->sendText(
            QString::fromUtf8(fixture(QStringLiteral("ws_syncplay_stop_after_join"))));
        QTRY_COMPARE(commands.count(), 1);
        QCOMPARE(updates.count(), 1);
        QCOMPARE(updates.first().first().value<GroupUpdate>().type, GroupUpdate::Type::GroupJoined);
        QCOMPARE(commands.first().first().value<Command>().kind, Command::Kind::Stop);
        for (const char *name :
             {"ws_syncplay_user_joined", "ws_syncplay_state_update", "ws_syncplay_play_queue",
              "ws_syncplay_group_left", "ws_syncplay_not_in_group"})
            connection->sendText(QString::fromUtf8(fixture(QString::fromLatin1(name))));
        QTRY_COMPARE(updates.count(), 6);
        QCOMPARE(updates.last().first().value<GroupUpdate>().type, GroupUpdate::Type::NotInGroup);

        // A frame that breaks a rule is dropped and the socket stays open
        QJsonObject command =
            QJsonDocument::fromJson(fixture(QStringLiteral("ws_syncplay_command")))
                .object()
                .value(QStringLiteral("Data"))
                .toObject();
        command.insert(QStringLiteral("GroupId"), QStringLiteral("../Items"));
        connection->sendText(enveloped(QStringLiteral("SyncPlayCommand"), command));
        const QJsonObject unknown{{QStringLiteral("GroupId"), QString(32, QLatin1Char('0'))},
                                  {QStringLiteral("Type"), QStringLiteral("GroupExploded")},
                                  {QStringLiteral("Data"), QString()}};
        connection->sendText(enveloped(QStringLiteral("SyncPlayGroupUpdate"), unknown));
        connection->sendText(QString::fromUtf8(fixture(QStringLiteral("ws_syncplay_command"))));
        QTRY_COMPARE(commands.count(), 2);
        QCOMPARE(commands.last().first().value<Command>().kind, Command::Kind::Unpause);
        QCOMPARE(updates.count(), 6);
        QVERIFY(connection->alive());
        QCOMPARE(flow.socket.state(), Kinefin::JellyfinSocketClient::State::Authenticated);
    }

    void routesGeneralCommand()
    {
        ConnectedSocket flow;
        QVERIFY(flow.connectAndAuthenticate());
        FakeSocketConnection *connection = flow.waitConnection();
        QVERIFY(connection);
        QSignalSpy commands(&flow.socket, &Kinefin::JellyfinSocketClient::generalCommand);
        connection->sendText(QString::fromUtf8(fixture(QStringLiteral("ws_general_command"))));
        QTRY_COMPARE(commands.count(), 1);
        const QList<QVariant> arguments = commands.takeFirst();
        QCOMPARE(arguments.first().toString(), QStringLiteral("DisplayMessage"));
        const QVariantMap map = arguments.last().toMap();
        QCOMPARE(map.count(), 2);
        QCOMPARE(map.value(QStringLiteral("Header")).toString(),
                 QStringLiteral("Repository restore finished"));
        QCOMPARE(map.value(QStringLiteral("Message")).toString(),
                 QStringLiteral("Media restore completed"));
        // Every argument must be a string, and the budgets of name, keys and sizes hold
        const QJsonObject nonString{{QStringLiteral("Name"), QStringLiteral("DisplayMessage")},
                                    {QStringLiteral("Arguments"),
                                     QJsonObject{{QStringLiteral("Message"), QJsonValue(12)}}}};
        connection->sendText(enveloped(QStringLiteral("GeneralCommand"), nonString));
        QJsonObject tooManyArg{{QStringLiteral("Name"), QStringLiteral("DisplayMessage")}};
        QJsonObject extra;
        for (int i = 0; i < 33; ++i)
            extra.insert(QString::number(i), QStringLiteral("x"));
        tooManyArg.insert(QStringLiteral("Arguments"), extra);
        connection->sendText(enveloped(QStringLiteral("GeneralCommand"), tooManyArg));
        const QJsonObject oversized{{QStringLiteral("Name"), QStringLiteral("DisplayMessage")},
                                    {QStringLiteral("Arguments"),
                                     QJsonObject{{QStringLiteral("Message"),
                                                  QJsonValue(QString(8193, QLatin1Char('x')))}}}};
        connection->sendText(enveloped(QStringLiteral("GeneralCommand"), oversized));
        const QJsonObject nameless{
            {QStringLiteral("Name"), QStringLiteral("has Digits and_!_dots")}};
        connection->sendText(enveloped(QStringLiteral("GeneralCommand"), nameless));
        QTest::qWait(100);
        QCOMPARE(commands.count(), 0);
    }

    void routesPlay()
    {
        ConnectedSocket flow;
        QVERIFY(flow.connectAndAuthenticate());
        FakeSocketConnection *connection = flow.waitConnection();
        QVERIFY(connection);
        QSignalSpy plays(&flow.socket, &Kinefin::JellyfinSocketClient::playRequested);
        connection->sendText(QString::fromUtf8(fixture(QStringLiteral("ws_play"))));
        QTRY_COMPARE(plays.count(), 1);
        const auto request = plays.takeFirst().first().value<Kinefin::RemotePlayRequest>();
        QCOMPARE(request.command, QStringLiteral("PlayNow"));
        QCOMPARE(request.itemIds, QStringList{QStringLiteral("c7dd3b1ac7dd3b1a9d42f2ab74f8a1b9")});
        // The live 12.4 shape carries no start ticks in the controller's default Play; the earlier fixture with StartPositionTicks stays as a valid variant (checked below)
        QCOMPARE(request.startPositionTicks, qint64(-1));
        const QJsonObject withTicks{{QStringLiteral("Command"), QStringLiteral("PlayNow")},
                                    {QStringLiteral("ItemIds"), QJsonArray{QJsonValue(kPlayItem)}},
                                    {QStringLiteral("StartPositionTicks"), QJsonValue(7)}};
        connection->sendText(enveloped(QStringLiteral("Play"), withTicks));
        QTRY_COMPARE(plays.count(), 1);
        QCOMPARE(plays.takeFirst().first().value<Kinefin::RemotePlayRequest>().startPositionTicks,
                 qint64(7));
        // Silent ticks are a valid variant; empty, invalid and oversized id lists are drops
        const QJsonObject unticked{{QStringLiteral("Command"), QStringLiteral("PlayNow")},
                                   {QStringLiteral("ItemIds"), QJsonArray{QJsonValue(kPlayItem)}}};
        connection->sendText(enveloped(QStringLiteral("Play"), unticked));
        QTRY_COMPARE(plays.count(), 1);
        QCOMPARE(plays.takeFirst().first().value<Kinefin::RemotePlayRequest>().startPositionTicks,
                 qint64(-1));
        const QJsonObject missingIds{{QStringLiteral("Command"), QStringLiteral("PlayNow")}};
        connection->sendText(enveloped(QStringLiteral("Play"), missingIds));
        const QJsonObject invalidId{
            {QStringLiteral("Command"), QStringLiteral("PlayNow")},
            {QStringLiteral("ItemIds"), QJsonArray{QJsonValue(QStringLiteral("..\\C:"))}}};
        connection->sendText(enveloped(QStringLiteral("Play"), invalidId));
        QJsonObject longList{{QStringLiteral("Command"), QStringLiteral("PlayNow")}};
        QJsonArray items;
        for (int i = 0; i < 1001; ++i)
            items.append(QJsonValue(kPlayItem));
        longList.insert(QStringLiteral("ItemIds"), items);
        connection->sendText(enveloped(QStringLiteral("Play"), longList));
        const QJsonObject badCommand{
            {QStringLiteral("Command"), QStringLiteral("not*a*command")},
            {QStringLiteral("ItemIds"), QJsonArray{QJsonValue(kPlayItem)}}};
        connection->sendText(enveloped(QStringLiteral("Play"), badCommand));
        QTest::qWait(100);
        QCOMPARE(plays.count(), 0);
        // StartIndex names the item to start at; one past the list names none, a non-integer is a drop
        const QJsonArray two{QJsonValue(kPlayItem), QJsonValue(kPlayItem)};
        const auto withIndex = [&two](const QJsonValue &index) {
            return QJsonObject{{QStringLiteral("PlayCommand"), QStringLiteral("PlayNow")},
                               {QStringLiteral("ItemIds"), two},
                               {QStringLiteral("StartIndex"), index}};
        };
        connection->sendText(enveloped(QStringLiteral("Play"), withIndex(1)));
        QTRY_COMPARE(plays.count(), 1);
        QCOMPARE(plays.takeFirst().first().value<Kinefin::RemotePlayRequest>().startIndex, 1);
        connection->sendText(enveloped(QStringLiteral("Play"), withIndex(2)));
        QTRY_COMPARE(plays.count(), 1);
        QCOMPARE(plays.takeFirst().first().value<Kinefin::RemotePlayRequest>().startIndex, -1);
        connection->sendText(enveloped(QStringLiteral("Play"), unticked));
        QTRY_COMPARE(plays.count(), 1);
        QCOMPARE(plays.takeFirst().first().value<Kinefin::RemotePlayRequest>().startIndex, -1);
        connection->sendText(enveloped(QStringLiteral("Play"), withIndex(-1)));
        connection->sendText(enveloped(QStringLiteral("Play"), withIndex(QStringLiteral("1"))));
        connection->sendText(enveloped(QStringLiteral("Play"), withIndex(0.5)));
        QTest::qWait(100);
        QCOMPARE(plays.count(), 0);
    }

    void routesSessionsOnlyWhenSubscribed()
    {
        ConnectedSocket flow;
        QVERIFY(flow.connectAndAuthenticate());
        FakeSocketConnection *connection = flow.waitConnection();
        QVERIFY(connection);
        QSignalSpy sessions(&flow.socket, &Kinefin::JellyfinSocketClient::sessionsUpdated);
        // Not subscribed: the broadcast arrives and goes nowhere
        connection->sendText(QString::fromUtf8(fixture(QStringLiteral("ws_sessions"))));
        QTest::qWait(100);
        QCOMPARE(sessions.count(), 0);
        // Subscribing sends the announce frame; one broadcast then routes
        flow.socket.setSessionsSubscription(true);
        QTRY_COMPARE(connection->receivedTexts.count(), 1);
        QCOMPARE(connection->receivedTexts.constLast(),
                 QStringLiteral("{\"MessageType\":\"SessionsStart\",\"Data\":\"0,1500\"}"));
        connection->sendText(QString::fromUtf8(fixture(QStringLiteral("ws_sessions"))));
        QTRY_COMPARE(sessions.count(), 1);
        QCOMPARE(sessions.first().first().value<QJsonArray>().count(), 1);
        // Unsubscribing announces the stop, then future broadcasts are ignored
        flow.socket.setSessionsSubscription(false);
        QTRY_COMPARE(connection->receivedTexts.count(), 2);
        QCOMPARE(connection->receivedTexts.constLast(),
                 QStringLiteral("{\"MessageType\":\"SessionsStop\"}"));
        connection->sendText(QString::fromUtf8(fixture(QStringLiteral("ws_sessions"))));
        QTest::qWait(100);
        QCOMPARE(sessions.count(), 1);
        // A non-array or non-object payload is a drop, never a close
        connection->sendText(enveloped(QStringLiteral("Sessions"), true));
        connection->sendText(enveloped(QStringLiteral("Sessions"), QJsonArray{42}));
        QTest::qWait(100);
        QCOMPARE(sessions.count(), 1);
        QVERIFY(connection->alive());
    }

    void routeUserDeletedTriggersVerify()
    {
        ConnectedSocket flow;
        QVERIFY(flow.connectAndAuthenticate());
        FakeSocketConnection *connection = flow.waitConnection();
        QVERIFY(connection);
        connection->sendText(QString::fromUtf8(fixture(QStringLiteral("ws_user_deleted"))));
        QTRY_VERIFY(flow.server.usersMeRequests >= 1);
        QVERIFY(flow.server.lastUsersMeAuth.contains(QByteArray("Token=")));
        // Another identity's deletion never touches our session
        const int seen = flow.server.usersMeRequests;
        connection->sendText(enveloped(QStringLiteral("UserDeleted"),
                                       QStringLiteral("00000000000000000000000000000001")));
        QTest::qWait(100);
        QCOMPARE(flow.server.usersMeRequests, seen);
        QCOMPARE(flow.socket.state(), Kinefin::JellyfinSocketClient::State::Authenticated);
    }

    void messageValidationDropsButKeepsConnection()
    {
        ConnectedSocket flow;
        QVERIFY(flow.connectAndAuthenticate());
        FakeSocketConnection *connection = flow.waitConnection();
        QVERIFY(connection);
        // Known-but-ignored and unknown types: no signal of any kind, connection intact
        connection->sendText(QString::fromUtf8(fixture(QStringLiteral("ws_server_restarting"))));
        connection->sendText(QString::fromUtf8(fixture(QStringLiteral("ws_keepalive_reply"))));
        connection->sendText(
            QStringLiteral("{\"MessageType\":\"SomethingTotallyNew\",\"Data\":{}}"));
        QTest::qWait(100);
        // Malformed frames: invalid JSON, a non-object, a missing or invalid type, flat data
        connection->sendText(QStringLiteral("not json at all"));
        connection->sendText(QStringLiteral("[1,2,3]"));
        connection->sendText(QStringLiteral("{\"Data\":{}}"));
        connection->sendText(QStringLiteral("{\"MessageType\":\"has$punct_chars\",\"Data\":{}}"));
        connection->sendText(QStringLiteral("{\"MessageType\":\"Playstate\",\"Data\":\"flat\"}"));
        QTest::qWait(100);
        // Valid traffic afterwards routes as usual, on the same connection
        QSignalSpy playstate(&flow.socket, &Kinefin::JellyfinSocketClient::playstateCommand);
        connection->sendText(
            QStringLiteral("{\"MessageType\":\"Playstate\",\"Data\":{\"Command\":\"Seek\"}}"));
        QTRY_COMPARE(playstate.count(), 1);
        QVERIFY(connection->alive());
        QCOMPARE(flow.server.upgradeAttempts, 1);
    }

    void oversizeMessageReconnects()
    {
        ConnectedSocket flow;
        QVERIFY(flow.connectAndAuthenticate());
        FakeSocketConnection *connection = flow.waitConnection();
        QVERIFY(connection);
        // One frame beyond the 4 MiB budget closes with 1009 and walks the reconnect ladder
        connection->sendBigText(qint64(4) * 1024 * 1024 + 1);
        QTRY_COMPARE(connection->clientCloseCodes.count(), 1);
        QCOMPARE(connection->clientCloseCodes.first(), int(1009));
        // The 1009 close walks the reconnect ladder: the same identity returns
        QTRY_VERIFY(flow.server.upgradeAttempts >= 2);
        QVERIFY(flow.waitState(Kinefin::JellyfinSocketClient::State::Authenticated, 3000));
        QCOMPARE(flow.socket.state(), Kinefin::JellyfinSocketClient::State::Authenticated);
    }

    void handshakeRejectedRevokedToken()
    {
        ConnectedSocket flow(0, 0, 3000, 6000);
        flow.server.socketUpgradeStatus = 403;
        flow.server.denyUsersMe = true;
        QVERIFY(flow.connectAndAuthenticate());
        // The re-check returns 401: the token expires, credentials are cleared and the ladder stops
        QSignalSpy expired(&flow.client, &Kinefin::JellyfinClient::sessionExpired);
        QSignalSpy cleared(&flow.client, &Kinefin::JellyfinClient::credentialsCleared);
        QVERIFY(flow.waitState(Kinefin::JellyfinSocketClient::State::Reconnecting, 3000));
        QTRY_COMPARE(expired.count(), 1);
        QTRY_COMPARE(cleared.count(), 1);
        QVERIFY(flow.waitState(Kinefin::JellyfinSocketClient::State::Disconnected, 3000));
        QCOMPARE(flow.server.upgradeAttempts, 1);
        QCOMPARE(cleared.count(), 1);
        QVERIFY(!flow.client.isAuthenticated());
    }

    void handshakeRejectedValidTokenBacksOff()
    {
        ConnectedSocket flow;
        flow.server.socketUpgradeStatus = 403;
        QVERIFY(flow.connectAndAuthenticate());
        QVERIFY(flow.waitState(Kinefin::JellyfinSocketClient::State::Reconnecting, 3000));
        // The token survives the re-check, so the ladder keeps climbing instead of stopping
        QSignalSpy cleared(&flow.client, &Kinefin::JellyfinClient::credentialsCleared);
        QVERIFY(QTest::qWaitFor([&flow] { return flow.server.upgradeAttempts >= 3; }, 3000));
        QCOMPARE(cleared.count(), 0);
    }

    void redirectNotFollowedIsAnAttemptFailure()
    {
        ConnectedSocket flow;
        flow.server.socketUpgradeStatus = 302;
        QVERIFY(flow.connectAndAuthenticate());
        QVERIFY(flow.waitState(Kinefin::JellyfinSocketClient::State::Reconnecting, 3000));
        // The 300 is never upgraded: a new plain attempt happens instead, and still no 101
        QTRY_VERIFY(flow.server.upgradeAttempts >= 2);
        QCOMPARE(flow.server.lastConnection(), nullptr);
        QVERIFY(flow.socket.state() != Kinefin::JellyfinSocketClient::State::Authenticated);
    }

    void logoutClosesSocket()
    {
        ConnectedSocket flow;
        QVERIFY(flow.connectAndAuthenticate());
        FakeSocketConnection *connection = flow.waitConnection();
        QVERIFY(connection);
        QSignalSpy cleared(&flow.client, &Kinefin::JellyfinClient::credentialsCleared);
        flow.client.disconnectFromServer();
        QCOMPARE(cleared.count(), 1);
        QTRY_COMPARE(connection->clientCloseCodes.count(), 1);
        QCOMPARE(connection->clientCloseCodes.first(), int(1000));
        QVERIFY(flow.waitState(Kinefin::JellyfinSocketClient::State::Disconnected, 3000));
        QCOMPARE(flow.socket.state(), Kinefin::JellyfinSocketClient::State::Disconnected);
    }

    void tokenRotationUsesNewToken()
    {
        ConnectedSocket flow;
        QVERIFY(flow.connectAndAuthenticate());
        FakeSocketConnection *connection = flow.waitConnection();
        QVERIFY(connection);
        QCOMPARE(flow.server.upgradeAttempts, 1);
        QVERIFY(flow.server.lastSocketAuth.contains(kFixtureToken));
        flow.client.disconnectFromServer();
        QVERIFY(flow.waitState(Kinefin::JellyfinSocketClient::State::Disconnected, 3000));
        flow.server.rotateSecondLoginToken = true;
        QVERIFY(login(flow.client, flow.server));
        // The fresh identity opens with the rotated token, never a stale copy
        QTRY_VERIFY(flow.server.upgradeAttempts >= 2);
        QByteArray rotatedToken = QByteArray("Token=\"") + flow.server.secondLoginToken;
        rotatedToken += "\"";
        QVERIFY(flow.server.lastSocketAuth.contains(rotatedToken));
        QVERIFY(flow.waitState(Kinefin::JellyfinSocketClient::State::Authenticated, 3000));
    }

    void serverCloseReconnects()
    {
        ConnectedSocket flow;
        QVERIFY(flow.connectAndAuthenticate());
        FakeSocketConnection *connection = flow.waitConnection();
        QVERIFY(connection);
        QCOMPARE(flow.server.upgradeAttempts, 1);
        connection->kick();
        if (!QTest::qWaitFor([&flow] { return flow.server.upgradeAttempts >= 2; }, 20000)) {
            qWarning() << "serverCloseReconnects: fake never counted attempt 2; client state"
                       << int(flow.socket.state()) << "usersMe" << flow.server.usersMeRequests
                       << "caps" << flow.server.capabilitiesRequests;
            QVERIFY(false);
        }
        QVERIFY(flow.waitState(Kinefin::JellyfinSocketClient::State::Authenticated, 20000));
    }

    void reconnectNowSkipsBackoff()
    {
        ConnectedSocket flow;
        QVERIFY(flow.connectAndAuthenticate());
        FakeSocketConnection *connection = flow.waitConnection();
        QVERIFY(connection);
        QCOMPARE(flow.server.upgradeAttempts, 1);
        // A long scheduled wait stays underneath an explicit user retry
        flow.socket.setTestTimings(0, 0, 0, 100000, 100000);
        connection->kick();
        QVERIFY(flow.waitState(Kinefin::JellyfinSocketClient::State::Reconnecting, 3000));
        QCOMPARE(flow.socket.state(), Kinefin::JellyfinSocketClient::State::Reconnecting);
        flow.socket.reconnectNow();
        QTRY_VERIFY(flow.server.upgradeAttempts >= 2);
        QVERIFY(flow.waitState(Kinefin::JellyfinSocketClient::State::Authenticated, 3000));
    }

    void logsNeverContainToken()
    {
        LogCapture capture;
        ConnectedSocket flow(0, 60);
        QVERIFY(flow.connectAndAuthenticate());
        FakeSocketConnection *connection = flow.waitConnection();
        QVERIFY(connection);
        // Exercise every path the logger touches: heartbeat, loss, reconnect, close
        connection->sendText(QString::fromUtf8(fixture(QStringLiteral("ws_force_keepalive"))));
        QTest::qWait(150);
        QTRY_VERIFY(flow.server.upgradeAttempts >= 2);
        flow.client.disconnectFromServer();
        QVERIFY(flow.waitState(Kinefin::JellyfinSocketClient::State::Disconnected, 3000));
        QVERIFY(!capture.messages.isEmpty());
        QVERIFY(capture.leakFree(kFixtureToken));
        QVERIFY(capture.leakFree("api_key=\""));
        QVERIFY(capture.leakFree("Token=\""));
    }

  private:
    class ConnectedSocket {
      public:
        FakeJellyfin server;
        Kinefin::JellyfinClient client;
        Kinefin::JellyfinSocketClient socket{&client};

        explicit ConnectedSocket(qint64 testConnectTimeout = 0, qint64 testDeadPeer = 0,
                                 qint64 testBackoffBase = 10, qint64 testBackoffCap = 40)
        {
            // Every live flow runs on the fast end of the ladder; the overrides land before login: timers already running would keep the production constant
            socket.setTestTimings(testConnectTimeout, testDeadPeer, 0, testBackoffBase,
                                  testBackoffCap);
        }

        bool connectAndAuthenticate() { return server.listen() && login(client, server); }

        // The WebSocket handshake is asynchronous: a game of `lastConnection()` before this wait would dereference a null pointer
        FakeSocketConnection *waitConnection(int ms = 5000)
        {
            if (!QTest::qWaitFor(
                    [&] {
                        return socket.state() == Kinefin::JellyfinSocketClient::State::Authenticated
                               && server.lastConnection() != nullptr;
                    },
                    ms))
                return nullptr;
            return server.lastConnection();
        }

        bool waitState(Kinefin::JellyfinSocketClient::State state, int ms = 5000) const
        {
            return QTest::qWaitFor([&] { return socket.state() == state; }, ms);
        }
    };
};

QTEST_MAIN(TestJellyfinSocketClient)
#include "test_jellyfin_socket.moc"
