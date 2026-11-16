// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Watching together against the fake server and its socket, with the real player and engine (vo=null, ao=null) on a generated clip in place of the server's stream: joining, the group's item opened paused and its Ready, pause, unpause and seek at server time, the user's actions turned into group requests, the frames that do nothing, the refusals and the end of a session
#include "FakeJellyfin.h"
#include "WavClip.h"

#include "api/JellyfinClient.h"
#include "api/JellyfinSocketClient.h"
#include "api/SyncPlayProtocol.h"
#include "models/PlayQueue.h"
#include "player/PlaybackEngine.h"
#include "player/PlayerController.h"
#include "settings/ClientSettings.h"
#include "syncplay/SyncPlayController.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using Kinefin::SyncPlayController;
namespace SyncPlay = Kinefin::SyncPlay;

namespace {

const QString kGroup = QStringLiteral("5f1e2d3c4b5a69788796a5b4c3d2e1f0");
const QString kOtherGroup = QStringLiteral("6f1e2d3c4b5a69788796a5b4c3d2e1f0");
const QString kEntry = QStringLiteral("9a8b7c6d5e4f40312a1b2c3d4e5f6071");
const QString kItem = QStringLiteral("c7dd3b1ac7dd3b1a9d42f2ab74f8a1b9");
const QString kOtherItem = QStringLiteral("d7dd3b1ac7dd3b1a9d42f2ab74f8a1b9");
constexpr qint64 kSecond = 10'000'000;

QString date(qint64 ms)
{
    return SyncPlay::serverDate(ms);
}

qint64 now()
{
    return QDateTime::currentMSecsSinceEpoch();
}

QString envelope(const QString &type, const QJsonObject &data)
{
    return QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("MessageType"), type},
                                                       {QStringLiteral("Data"), data}})
                                 .toJson(QJsonDocument::Compact));
}

QString command(const QString &kind, qint64 whenMs, qint64 emittedAtMs, qint64 positionTicks,
                const QString &group = kGroup, const QString &entry = kEntry)
{
    return envelope(QStringLiteral("SyncPlayCommand"),
                    {{QStringLiteral("GroupId"), group},
                     {QStringLiteral("PlaylistItemId"), entry},
                     {QStringLiteral("When"), date(whenMs)},
                     {QStringLiteral("PositionTicks"), positionTicks},
                     {QStringLiteral("Command"), kind},
                     {QStringLiteral("EmittedAt"), date(emittedAtMs)}});
}

QString update(const QString &type, const QJsonValue &payload, const QString &group = kGroup)
{
    return envelope(QStringLiteral("SyncPlayGroupUpdate"), {{QStringLiteral("GroupId"), group},
                                                            {QStringLiteral("Type"), type},
                                                            {QStringLiteral("Data"), payload}});
}

QJsonObject groupInfo(const QString &group = kGroup)
{
    return {{QStringLiteral("GroupId"), group},
            {QStringLiteral("GroupName"), QStringLiteral("Movie night")},
            {QStringLiteral("State"), QStringLiteral("Idle")},
            {QStringLiteral("Participants"), QJsonArray{QStringLiteral("alice")}},
            {QStringLiteral("LastUpdatedAt"), date(now())}};
}

QJsonObject playQueue(const QString &item, qint64 startTicks, qint64 lastUpdateMs)
{
    return {{QStringLiteral("Reason"), QStringLiteral("NewPlaylist")},
            {QStringLiteral("LastUpdate"), date(lastUpdateMs)},
            {QStringLiteral("Playlist"),
             QJsonArray{QJsonObject{{QStringLiteral("ItemId"), item},
                                    {QStringLiteral("PlaylistItemId"), kEntry}}}},
            {QStringLiteral("PlayingItemIndex"), 0},
            {QStringLiteral("StartPositionTicks"), startTicks},
            {QStringLiteral("IsPlaying"), false},
            {QStringLiteral("ShuffleMode"), QStringLiteral("Sorted")},
            {QStringLiteral("RepeatMode"), QStringLiteral("RepeatNone")}};
}

QString settingsFile(const QTemporaryDir &dir, const QByteArray &conf)
{
    const QString path = dir.filePath(QStringLiteral("kinefin.conf"));
    QFile file(path);
    if (!conf.isEmpty() && (!file.open(QIODevice::WriteOnly) || file.write(conf) != conf.size()))
        return {};
    return path;
}

struct Group {
    explicit Group(const QByteArray &conf = {})
        : queue(&client), settings(settingsFile(dir, conf)),
          player(client, queue, settings, QString()), syncPlay(client, socket, player)
    {
        player.setGroupPlayback(&syncPlay);
        syncPlay.setTestTimings(800, 100, 60'000);
        server.utcTimeNow = true;
        // The source comes from the test, a clip on disk: PlaybackInfo is never answered
        server.holdPlaybackInfo = true;
        ok = server.listen() && login(client, server)
             && engine()->init({QStringLiteral("vo=null"), QStringLiteral("ao=null")})
             && QTest::qWaitFor(
                 [this] {
                     return socket.state() == Kinefin::JellyfinSocketClient::State::Authenticated
                            && server.lastConnection();
                 },
                 5000);
    }
    Kinefin::PlaybackEngine *engine() { return player.findChild<Kinefin::PlaybackEngine *>(); }
    void push(const QString &frame) { server.lastConnection()->sendText(frame); }
    // The requests of one route the fake saw, in order
    QList<QJsonObject> sent(const QString &route) const
    {
        QList<QJsonObject> bodies;
        for (const auto &request : server.syncPlayRequests) {
            if (request.route == route)
                bodies.append(request.body);
        }
        return bodies;
    }
    // Joined, with the Stop of every join and a first clock sample
    bool joined(qint64 joinStopEmittedAt = 0)
    {
        if (!syncPlay.joinGroup(kGroup)
            || !QTest::qWaitFor([this] { return !sent(QStringLiteral("Join")).isEmpty(); }))
            return false;
        push(update(QStringLiteral("GroupJoined"), groupInfo()));
        const qint64 emitted = joinStopEmittedAt > 0 ? joinStopEmittedAt : now();
        push(command(QStringLiteral("Stop"), emitted - 10'000, emitted, 0, kGroup,
                     QString(32, QLatin1Char('0'))));
        return QTest::qWaitFor([this] {
            return syncPlay.status() == SyncPlayController::Status::Joined
                   && !sent(QStringLiteral("Ping")).isEmpty();
        });
    }
    // Hands the player the clip of `seconds` for the item it is loading
    bool sourceGiven(const QString &item, int seconds = 10)
    {
        if (!QTest::qWaitFor([&] { return player.itemId() == item && player.loading(); }))
            return false;
        Kinefin::PlaybackSource source;
        source.itemId = item;
        source.mediaSourceId = QStringLiteral("c1b2a3d4e5f6071829304a5b6c7d8e9f");
        source.playSessionId = QStringLiteral("s1");
        source.method = Kinefin::PlaybackSource::Method::Direct;
        source.url = QUrl(TestWav::writeWavClip(dir, seconds));
        Q_EMIT client.playbackInfoReady(source);
        return true;
    }
    // The group's queue names the item; the player opens it paused and says Ready
    bool queueOpened(qint64 startTicks = 0)
    {
        push(update(QStringLiteral("PlayQueue"), playQueue(kItem, startTicks, now())));
        return sourceGiven(kItem)
               && QTest::qWaitFor([this] { return !sent(QStringLiteral("Ready")).isEmpty(); },
                                  10000);
    }
    // Opened and playing from 0 through an Unpause due now
    bool playing()
    {
        if (!queueOpened())
            return false;
        const qint64 at = now();
        push(command(QStringLiteral("Unpause"), at, at, 0));
        return QTest::qWaitFor([this] { return !player.paused() && player.active(); });
    }

    QTemporaryDir dir;
    FakeJellyfin server;
    Kinefin::JellyfinClient client;
    Kinefin::JellyfinSocketClient socket{&client};
    Kinefin::PlayQueue queue;
    Kinefin::ClientSettings settings;
    Kinefin::PlayerController player;
    SyncPlayController syncPlay;
    bool ok = false;
};

} // namespace

class TestSyncPlayController : public QObject {
    Q_OBJECT

  private Q_SLOTS:
    void aJoinThenTheQueueOpensPausedAndSaysReady()
    {
        Group group("[video]\ndefault_playback_speed=1.5\n");
        QVERIFY(group.ok);
        QSignalSpy status(&group.syncPlay, &SyncPlayController::statusChanged);
        QVERIFY(group.joined());
        QCOMPARE(group.sent(QStringLiteral("Join")).first(), SyncPlay::joinBody(kGroup));
        QCOMPARE(group.syncPlay.groupName(), QStringLiteral("Movie night"));
        QCOMPARE(group.syncPlay.participants(), QStringList{QStringLiteral("alice")});
        QVERIFY(group.server.utcTimeRequests >= 1);
        QVERIFY(group.sent(QStringLiteral("Ping")).first().value(QStringLiteral("Ping")).toInt()
                >= 0);
        QVERIFY(group.queueOpened());
        const QJsonObject ready = group.sent(QStringLiteral("Ready")).first();
        QCOMPARE(ready.value(QStringLiteral("PlaylistItemId")).toString(), kEntry);
        QCOMPARE(ready.value(QStringLiteral("IsPlaying")).toBool(), false);
        QVERIFY(ready.value(QStringLiteral("PositionTicks")).toDouble() < kSecond);
        const qint64 when =
            SyncPlay::parseServerDateMs(ready.value(QStringLiteral("When")).toString()).value_or(0);
        QVERIFY(std::abs(when - now()) < 2000);
        QVERIFY(group.player.paused());
        // A group plays at 1.0, whatever the speed setting says
        QCOMPARE(group.player.rate(), 1.0);
        // No queue of its own: the saved queue is not the group's
        QCOMPARE(group.player.queueCount(), 0);
    }

    void joiningMidItemSaysReadyWhereTheGroupIs()
    {
        Group group;
        QVERIFY(group.ok);
        QVERIFY(group.joined());
        QVERIFY(group.queueOpened(3 * kSecond));
        const QJsonObject ready = group.sent(QStringLiteral("Ready")).first();
        QVERIFY(std::abs(ready.value(QStringLiteral("PositionTicks")).toDouble() - 3 * kSecond)
                < kSecond / 5);
        QVERIFY(group.player.paused());
    }

    void theSpeedStaysWhenASettingChanges()
    {
        Group group("[video]\ndefault_playback_speed=1.5\n");
        QVERIFY(group.ok);
        QVERIFY(group.joined());
        QVERIFY(group.playing());
        QVERIFY(group.settings.setValue(QStringLiteral("video"), QStringLiteral("cache"), 150));
        QTest::qWait(200);
        QCOMPARE(group.player.rate(), 1.0);
        // Left, the playback is the user's own and the setting applies again
        group.syncPlay.leaveGroup();
        QVERIFY(group.settings.setValue(QStringLiteral("video"), QStringLiteral("cache"), 500));
        QTRY_COMPARE(group.player.rate(), 1.5);
    }

    void aQueueEditKeepsAWaitingUnpause()
    {
        Group group;
        QVERIFY(group.ok);
        QVERIFY(group.joined());
        QVERIFY(group.queueOpened());
        const qint64 at = now();
        group.push(command(QStringLiteral("Unpause"), at + 500, at, 0));
        group.push(update(QStringLiteral("PlayQueue"), playQueue(kItem, 0, at + 1)));
        QTRY_VERIFY_WITH_TIMEOUT(!group.player.paused(), 3000);
    }

    void heldCommandsExpire()
    {
        Group group;
        QVERIFY(group.ok);
        group.server.utcTimeStatus = 503;
        QVERIFY(group.syncPlay.joinGroup(kGroup));
        group.push(update(QStringLiteral("GroupJoined"), groupInfo()));
        QTRY_COMPARE(group.syncPlay.status(), SyncPlayController::Status::Joined);
        group.push(update(QStringLiteral("PlayQueue"), playQueue(kItem, 0, now())));
        QVERIFY(group.sourceGiven(kItem));
        QTRY_VERIFY_WITH_TIMEOUT(group.player.paused(), 5000);
        const qint64 at = now();
        group.push(command(QStringLiteral("Unpause"), at, at, 0));
        // Past the two seconds a command may wait for the clock
        QTest::qWait(2400);
        group.server.utcTimeStatus = 200;
        QTRY_COMPARE_WITH_TIMEOUT(group.sent(QStringLiteral("Ready")).size(), 1, 3000);
        QTest::qWait(300);
        QVERIFY(group.player.paused());
    }

    void anUnpauseStartsAtServerTime()
    {
        Group group;
        QVERIFY(group.ok);
        QVERIFY(group.joined());
        QVERIFY(group.queueOpened());
        const qint64 at = now();
        group.push(command(QStringLiteral("Unpause"), at + 600, at, 0));
        QElapsedTimer clock;
        clock.start();
        QTRY_VERIFY_WITH_TIMEOUT(!group.player.paused(), 3000);
        const qint64 waited = clock.elapsed();
        QVERIFY2(waited >= 450 && waited <= 900, qPrintable(QString::number(waited)));
    }

    void aLateUnpausePlaysFromWhereTheGroupIs()
    {
        Group group;
        QVERIFY(group.ok);
        QVERIFY(group.joined());
        QVERIFY(group.queueOpened());
        const qint64 at = now();
        group.push(command(QStringLiteral("Unpause"), at - 3000, at, 0));
        QTRY_VERIFY_WITH_TIMEOUT(!group.player.paused(), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(group.player.positionTicks() >= 28 * kSecond / 10, 3000);
    }

    void aPauseStopsWhereTheGroupStopped()
    {
        Group group;
        QVERIFY(group.ok);
        QVERIFY(group.joined());
        QVERIFY(group.playing());
        const qint64 at = now();
        group.push(command(QStringLiteral("Pause"), at, at, 5 * kSecond));
        QTRY_VERIFY(group.player.paused());
        QTRY_VERIFY_WITH_TIMEOUT(std::abs(group.player.positionTicks() - 5 * kSecond) < kSecond / 5,
                                 3000);
    }

    void aSeekPausesThenSaysReadyThere()
    {
        Group group;
        QVERIFY(group.ok);
        QVERIFY(group.joined());
        QVERIFY(group.playing());
        const qint64 at = now();
        group.push(command(QStringLiteral("Seek"), at, at, 4 * kSecond));
        QTRY_COMPARE_WITH_TIMEOUT(group.sent(QStringLiteral("Ready")).size(), 2, 5000);
        QVERIFY(group.player.paused());
        const QJsonObject ready = group.sent(QStringLiteral("Ready")).last();
        QVERIFY(std::abs(ready.value(QStringLiteral("PositionTicks")).toDouble() - 4 * kSecond)
                < kSecond / 5);
        QCOMPARE(ready.value(QStringLiteral("IsPlaying")).toBool(), false);
    }

    void theUsersActionsGoToTheGroup()
    {
        Group group;
        QVERIFY(group.ok);
        QVERIFY(group.joined());
        QVERIFY(group.playing());
        group.player.pause();
        QTRY_COMPARE(group.sent(QStringLiteral("Pause")).size(), 1);
        // Nothing moves until the group says so
        QVERIFY(!group.player.paused());
        group.player.seek(5 * kSecond);
        QTRY_COMPARE(group.sent(QStringLiteral("Seek")).size(), 1);
        QCOMPARE(group.sent(QStringLiteral("Seek")).first(), SyncPlay::seekBody(5 * kSecond));
        QVERIFY(group.player.playItem(kOtherItem, 2 * kSecond));
        QTRY_COMPARE(group.sent(QStringLiteral("SetNewQueue")).size(), 1);
        QCOMPARE(group.sent(QStringLiteral("SetNewQueue")).first(),
                 SyncPlay::newQueueBody({kOtherItem}, 0, 2 * kSecond));
        QCOMPARE(group.player.itemId(), kItem);
        group.player.setRate(2.0);
        QTest::qWait(100);
        QCOMPARE(group.player.rate(), 1.0);
        QVERIFY(!group.player.next());
        QVERIFY(!group.player.previous());
    }

    void aRequestTheServerRefusesIsSaid()
    {
        Group group;
        QVERIFY(group.ok);
        QVERIFY(group.joined());
        QVERIFY(group.playing());
        QSignalSpy failed(&group.syncPlay, &SyncPlayController::failed);
        group.server.syncPlayStatus = 500;
        group.player.pause();
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(failed.first().first().value<SyncPlayController::Failure>(),
                 SyncPlayController::Failure::RequestFailed);
        // The group goes on
        QCOMPARE(group.syncPlay.status(), SyncPlayController::Status::Joined);
    }

    void quickActionsKeepOnlyTheLatest()
    {
        Group group;
        QVERIFY(group.ok);
        QVERIFY(group.joined());
        QVERIFY(group.playing());
        group.server.holdSyncPlay = true;
        group.player.pause();
        QTRY_COMPARE(group.server.heldCount(), 1);
        group.player.resume();
        group.player.seek(2 * kSecond);
        group.player.seek(3 * kSecond);
        group.server.holdSyncPlay = false;
        group.server.releaseHeld();
        QTRY_COMPARE(group.sent(QStringLiteral("Seek")).size(), 1);
        QTest::qWait(100);
        QCOMPARE(group.sent(QStringLiteral("Pause")).size(), 1);
        QCOMPARE(group.sent(QStringLiteral("Unpause")).size(), 0);
        QCOMPARE(group.sent(QStringLiteral("Seek")).first(), SyncPlay::seekBody(3 * kSecond));
    }

    void framesThatDoNothing()
    {
        Group group;
        QVERIFY(group.ok);
        // The join's Stop came two minutes ago, so a later command can still be stale
        QVERIFY(group.joined(now() - 120'000));
        QVERIFY(group.queueOpened());
        const qint64 stale = now() - 90'000;
        group.push(command(QStringLiteral("Unpause"), stale, stale, 0));
        QTest::qWait(300);
        QVERIFY(group.player.paused());
        const qint64 at = now();
        group.push(command(QStringLiteral("Unpause"), at, at, 0));
        QTRY_VERIFY(!group.player.paused());
        // Another group, an older command, an entry not loaded here, another group's state
        group.push(command(QStringLiteral("Pause"), at, at + 1, 0, kOtherGroup));
        group.push(command(QStringLiteral("Pause"), at, at - 1, 0));
        group.push(
            command(QStringLiteral("Pause"), at, at, 0, kGroup, QString(32, QLatin1Char('a'))));
        group.push(update(QStringLiteral("StateUpdate"),
                          QJsonObject{{QStringLiteral("State"), QStringLiteral("Waiting")},
                                      {QStringLiteral("Reason"), QStringLiteral("Seek")}},
                          kOtherGroup));
        QTest::qWait(300);
        QVERIFY(!group.player.paused());
        QVERIFY(!group.syncPlay.waiting());
        // Commands may share a millisecond: one that differs from the last still counts
        group.push(command(QStringLiteral("Pause"), at, at, 0));
        QTRY_VERIFY(group.player.paused());
    }

    void theStopOfAJoinEndsWhatPlayedAlone()
    {
        Group group;
        QVERIFY(group.ok);
        QVERIFY(group.player.playItem(kOtherItem));
        QVERIFY(group.sourceGiven(kOtherItem));
        QTRY_VERIFY_WITH_TIMEOUT(group.player.active() && !group.player.loading(), 5000);
        QVERIFY(group.joined());
        QTRY_VERIFY_WITH_TIMEOUT(!group.player.active(), 5000);
    }

    void anEmptyQueueStopsTheGroupsItem()
    {
        Group group;
        QVERIFY(group.ok);
        QVERIFY(group.joined());
        QVERIFY(group.playing());
        QJsonObject empty = playQueue(kItem, 0, now() + 1);
        empty.insert(QStringLiteral("Playlist"), QJsonArray());
        empty.insert(QStringLiteral("PlayingItemIndex"), -1);
        group.push(update(QStringLiteral("PlayQueue"), empty));
        QTRY_VERIFY_WITH_TIMEOUT(!group.player.active(), 5000);
    }

    void aCommandBeforeTheClockWaitsForIt()
    {
        Group group;
        QVERIFY(group.ok);
        group.server.utcTimeStatus = 503;
        QVERIFY(group.syncPlay.joinGroup(kGroup));
        group.push(update(QStringLiteral("GroupJoined"), groupInfo()));
        QTRY_COMPARE(group.syncPlay.status(), SyncPlayController::Status::Joined);
        group.push(update(QStringLiteral("PlayQueue"), playQueue(kItem, 0, now())));
        QVERIFY(group.sourceGiven(kItem));
        QTRY_VERIFY_WITH_TIMEOUT(group.player.paused(), 5000);
        // No Ready without the server's time
        QTest::qWait(200);
        QVERIFY(group.sent(QStringLiteral("Ready")).isEmpty());
        const qint64 at = now();
        group.push(command(QStringLiteral("Unpause"), at, at, 0));
        QTest::qWait(150);
        QVERIFY(group.player.paused());
        group.server.utcTimeStatus = 200;
        QTRY_VERIFY_WITH_TIMEOUT(!group.player.paused(), 2000);
        QTRY_COMPARE(group.sent(QStringLiteral("Ready")).size(), 1);
        QCOMPARE(
            group.sent(QStringLiteral("Ready")).first().value(QStringLiteral("IsPlaying")).toBool(),
            false);
    }

    void membersComeAndGo()
    {
        Group group;
        QVERIFY(group.ok);
        QVERIFY(group.joined());
        QSignalSpy joined(&group.syncPlay, &SyncPlayController::memberJoined);
        QSignalSpy left(&group.syncPlay, &SyncPlayController::memberLeft);
        group.push(update(QStringLiteral("UserJoined"), QStringLiteral("bob")));
        QTRY_COMPARE(joined.size(), 1);
        QCOMPARE(joined.first().first().toString(), QStringLiteral("bob"));
        QCOMPARE(group.syncPlay.participants(),
                 (QStringList{QStringLiteral("alice"), QStringLiteral("bob")}));
        group.push(update(QStringLiteral("UserLeft"), QStringLiteral("bob")));
        QTRY_COMPARE(left.size(), 1);
        QCOMPARE(group.syncPlay.participants(), QStringList{QStringLiteral("alice")});
        group.push(update(QStringLiteral("StateUpdate"),
                          QJsonObject{{QStringLiteral("State"), QStringLiteral("Waiting")},
                                      {QStringLiteral("Reason"), QStringLiteral("Buffer")}}));
        QTRY_VERIFY(group.syncPlay.waiting());
    }

    void refusalsWhileJoining_data()
    {
        QTest::addColumn<QString>("type");
        QTest::addColumn<bool>("create");
        QTest::addColumn<int>("failure");
        QTest::newRow("group gone") << QStringLiteral("GroupDoesNotExist") << false
                                    << int(SyncPlayController::Failure::GroupGone);
        QTest::newRow("join denied") << QStringLiteral("JoinGroupDenied") << false
                                     << int(SyncPlayController::Failure::JoinDenied);
        QTest::newRow("create denied") << QStringLiteral("CreateGroupDenied") << true
                                       << int(SyncPlayController::Failure::CreateDenied);
        QTest::newRow("library") << QStringLiteral("LibraryAccessDenied") << false
                                 << int(SyncPlayController::Failure::LibraryAccessDenied);
    }
    void refusalsWhileJoining()
    {
        QFETCH(QString, type);
        QFETCH(bool, create);
        QFETCH(int, failure);
        Group group;
        QVERIFY(group.ok);
        QSignalSpy failed(&group.syncPlay, &SyncPlayController::failed);
        QVERIFY(create ? group.syncPlay.createGroup(QStringLiteral("Movie night"))
                       : group.syncPlay.joinGroup(kGroup));
        QCOMPARE(group.syncPlay.status(), SyncPlayController::Status::Joining);
        group.push(update(type, QString(), QString(32, QLatin1Char('0'))));
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(int(failed.first().first().value<SyncPlayController::Failure>()), failure);
        QCOMPARE(group.syncPlay.status(), SyncPlayController::Status::Idle);
        QVERIFY(!group.syncPlay.routesPlayback());
    }

    void aRefusedRequestOrNoAnswerEndsTheJoin()
    {
        Group group;
        QVERIFY(group.ok);
        QSignalSpy failed(&group.syncPlay, &SyncPlayController::failed);
        group.server.syncPlayStatus = 403;
        QVERIFY(group.syncPlay.joinGroup(kGroup));
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(failed.first().first().value<SyncPlayController::Failure>(),
                 SyncPlayController::Failure::JoinDenied);
        group.server.syncPlayStatus = 204;
        // The server took the request but never said GroupJoined: the join gives up and leaves
        QVERIFY(group.syncPlay.createGroup(QStringLiteral("Movie night")));
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 2, 3000);
        QCOMPARE(failed.last().first().value<SyncPlayController::Failure>(),
                 SyncPlayController::Failure::NoAnswer);
        QTRY_COMPARE(group.sent(QStringLiteral("Leave")).size(), 1);
        // A join that names a different group is not this one's answer
        QVERIFY(group.syncPlay.joinGroup(kGroup));
        group.push(update(QStringLiteral("GroupJoined"), groupInfo(kOtherGroup), kOtherGroup));
        QTest::qWait(200);
        QCOMPARE(group.syncPlay.status(), SyncPlayController::Status::Joining);
    }

    void theGroupEnds()
    {
        Group group;
        QVERIFY(group.ok);
        QSignalSpy failed(&group.syncPlay, &SyncPlayController::failed);
        QVERIFY(group.joined());
        group.push(update(QStringLiteral("GroupLeft"),
                          QStringLiteral("5f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0")));
        QTRY_COMPARE(group.syncPlay.status(), SyncPlayController::Status::Idle);
        QCOMPARE(failed.size(), 0);
        QVERIFY(group.joined());
        group.push(update(QStringLiteral("NotInGroup"), QString(), QString(32, QLatin1Char('0'))));
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(failed.first().first().value<SyncPlayController::Failure>(),
                 SyncPlayController::Failure::GroupGone);
        // Denied the group's item while in it: the session leaves, as the server may still count it
        QVERIFY(group.joined());
        const qsizetype leaves = group.sent(QStringLiteral("Leave")).size();
        group.push(update(QStringLiteral("LibraryAccessDenied"), QString(),
                          QString(32, QLatin1Char('0'))));
        QTRY_COMPARE(failed.size(), 2);
        QTRY_COMPARE(group.sent(QStringLiteral("Leave")).size(), leaves + 1);
    }

    void leavingIsLocalAndAtOnce()
    {
        Group group;
        QVERIFY(group.ok);
        QVERIFY(group.joined());
        QVERIFY(group.playing());
        group.syncPlay.leaveGroup();
        QCOMPARE(group.syncPlay.status(), SyncPlayController::Status::Idle);
        QTRY_COMPARE(group.sent(QStringLiteral("Leave")).size(), 1);
        // The playback goes on, the user's own again
        group.player.pause();
        QTRY_VERIFY(group.player.paused());
        QCOMPARE(group.sent(QStringLiteral("Pause")).size(), 0);
    }

    void theSessionsEndSendsNothing()
    {
        Group group;
        QVERIFY(group.ok);
        QVERIFY(group.joined());
        const qsizetype requests = group.server.syncPlayRequests.size();
        group.client.disconnectFromServer();
        QCOMPARE(group.syncPlay.status(), SyncPlayController::Status::Idle);
        QTest::qWait(400);
        QCOMPARE(group.server.syncPlayRequests.size(), requests);
        QVERIFY(!group.syncPlay.joinGroup(kGroup));
    }

    void nothingStartsWithoutTheSocket()
    {
        QTemporaryDir dir;
        FakeJellyfin server;
        Kinefin::JellyfinClient client;
        Kinefin::JellyfinSocketClient socket(&client);
        Kinefin::PlayQueue queue(&client);
        Kinefin::ClientSettings settings(settingsFile(dir, {}));
        Kinefin::PlayerController player(client, queue, settings, QString());
        SyncPlayController syncPlay(client, socket, player);
        QVERIFY(!syncPlay.joinGroup(kGroup));
        server.holdSocketUpgrade = true;
        QVERIFY(server.listen() && login(client, server));
        QVERIFY(!syncPlay.createGroup(QStringLiteral("Movie night")));
        QVERIFY(!syncPlay.joinGroup(QStringLiteral("../Items")));
        QTest::qWait(100);
        QVERIFY(server.syncPlayRequests.isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestSyncPlayController)
#include "test_syncplay_controller.moc"
