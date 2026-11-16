// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The SyncPlay wire format: the frames and replies observed on 12.4 (the fixtures), every rule that drops one, the request bodies against their goldens and the validator, then the client's SyncPlay requests against the fake server
#include "FakeJellyfin.h"

#include "api/JellyfinClient.h"
#include "api/SyncPlayProtocol.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTimeZone>
#include <QtTest>

namespace SyncPlay = Kinefin::SyncPlay;

namespace {

const QString kGroup = QStringLiteral("5f1e2d3c4b5a69788796a5b4c3d2e1f0");
const QString kEntry = QStringLiteral("9a8b7c6d5e4f40312a1b2c3d4e5f6071");
const QString kItem = QStringLiteral("c7dd3b1ac7dd3b1a9d42f2ab74f8a1b9");
const QString kZero = QString(32, QLatin1Char('0'));

QJsonObject fixtureObject(const QString &name)
{
    return QJsonDocument::fromJson(fixture(name)).object();
}

// The `Data` of a socket fixture
QJsonValue frameData(const QString &name)
{
    return fixtureObject(name).value(QStringLiteral("Data"));
}

QJsonObject withValue(QJsonObject object, const QString &key, const QJsonValue &value)
{
    object.insert(key, value);
    return object;
}

// A group update frame's `Data` with its inner `Data` replaced
QJsonObject withPayload(const QString &name, const QJsonValue &payload)
{
    return withValue(frameData(name).toObject(), QStringLiteral("Data"), payload);
}

qint64 utcMs(int year, int month, int day, int hour, int minute, int second, int ms)
{
    return QDateTime(QDate(year, month, day), QTime(hour, minute, second, ms), QTimeZone::UTC)
        .toMSecsSinceEpoch();
}

} // namespace

class TestSyncPlayProtocol : public QObject {
    Q_OBJECT

  private Q_SLOTS:
    void serverDates_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<qint64>("expected");
        const qint64 base = utcMs(2026, 11, 16, 16, 37, 59, 0);
        QTest::newRow("seven digits")
            << QStringLiteral("2026-11-16T16:37:59.8904746Z") << base + 890;
        QTest::newRow("six digits") << QStringLiteral("2026-11-16T16:37:59.068638Z") << base + 68;
        QTest::newRow("one digit") << QStringLiteral("2026-11-16T16:37:59.5Z") << base + 500;
        QTest::newRow("none") << QStringLiteral("2026-11-16T16:37:59Z") << base;
        QTest::newRow("leap day") << QStringLiteral("2028-02-29T00:00:00Z")
                                  << utcMs(2028, 2, 29, 0, 0, 0, 0);
    }
    void serverDates()
    {
        QFETCH(QString, text);
        QFETCH(qint64, expected);
        QCOMPARE(SyncPlay::parseServerDateMs(text), std::optional<qint64>(expected));
    }

    void badServerDates_data()
    {
        QTest::addColumn<QString>("text");
        QTest::newRow("offset") << QStringLiteral("2026-11-16T16:37:59.890+00:00");
        QTest::newRow("no zone") << QStringLiteral("2026-11-16T16:37:59.890");
        QTest::newRow("eight digits") << QStringLiteral("2026-11-16T16:37:59.89047461Z");
        QTest::newRow("empty fraction") << QStringLiteral("2026-11-16T16:37:59.Z");
        QTest::newRow("month 13") << QStringLiteral("2026-13-16T16:37:59Z");
        QTest::newRow("30 February") << QStringLiteral("2026-02-30T16:37:59Z");
        QTest::newRow("hour 24") << QStringLiteral("2026-11-16T24:00:00Z");
        QTest::newRow("space") << QStringLiteral("2026-11-16 16:37:59Z");
        QTest::newRow("arabic digits") << QStringLiteral("٢٠٢٦-11-16T16:37:59Z");
        QTest::newRow("trailing text") << QStringLiteral("2026-11-16T16:37:59Zx");
        QTest::newRow("trailing newline") << QStringLiteral("2026-11-16T16:37:59Z\n");
        QTest::newRow("empty") << QString();
    }
    void badServerDates()
    {
        QFETCH(QString, text);
        QVERIFY(!SyncPlay::parseServerDateMs(text));
    }

    void serverDateRoundTrip()
    {
        const qint64 ms = utcMs(2026, 11, 16, 16, 37, 57, 317);
        QCOMPARE(SyncPlay::serverDate(ms), QStringLiteral("2026-11-16T16:37:57.3170000Z"));
        QCOMPARE(SyncPlay::parseServerDateMs(SyncPlay::serverDate(ms)), std::optional<qint64>(ms));
    }

    void commandFromTheServer()
    {
        const auto command =
            SyncPlay::parseCommand(frameData(QStringLiteral("ws_syncplay_command")));
        QVERIFY(command);
        QCOMPARE(command->kind, SyncPlay::Command::Kind::Unpause);
        QCOMPARE(command->groupId, kGroup);
        QCOMPARE(command->playlistItemId, kEntry);
        QCOMPARE(command->positionTicks, qint64(0));
        QCOMPARE(command->whenMs, utcMs(2026, 11, 16, 16, 37, 59, 890));
        QCOMPARE(command->emittedAtMs, utcMs(2026, 11, 16, 16, 37, 58, 890));
    }

    void stopAfterAJoinHasNoEntry()
    {
        const auto command =
            SyncPlay::parseCommand(frameData(QStringLiteral("ws_syncplay_stop_after_join")));
        QVERIFY(command);
        QCOMPARE(command->kind, SyncPlay::Command::Kind::Stop);
        QVERIFY(command->playlistItemId.isEmpty());
    }

    void commandIdsAreCanonical()
    {
        const QJsonObject data = frameData(QStringLiteral("ws_syncplay_command")).toObject();
        const QString dashedUpper = QStringLiteral("5F1E2D3C-4B5A-6978-8796-A5B4C3D2E1F0");
        const auto command =
            SyncPlay::parseCommand(withValue(data, QStringLiteral("GroupId"), dashedUpper));
        QVERIFY(command);
        QCOMPARE(command->groupId, kGroup);
    }

    void commandsThatAreDropped_data()
    {
        QTest::addColumn<QString>("key");
        QTest::addColumn<QJsonValue>("value");
        QTest::newRow("unknown command")
            << QStringLiteral("Command") << QJsonValue(QStringLiteral("Rewind"));
        QTest::newRow("lower-case command")
            << QStringLiteral("Command") << QJsonValue(QStringLiteral("pause"));
        QTest::newRow("negative position") << QStringLiteral("PositionTicks") << QJsonValue(-1);
        QTest::newRow("fractional position") << QStringLiteral("PositionTicks") << QJsonValue(1.5);
        QTest::newRow("position past 24 h") << QStringLiteral("PositionTicks")
                                            << QJsonValue(double(SyncPlay::MaxPositionTicks + 1));
        QTest::newRow("position as text")
            << QStringLiteral("PositionTicks") << QJsonValue(QStringLiteral("0"));
        QTest::newRow("path in the group id")
            << QStringLiteral("GroupId") << QJsonValue(QStringLiteral("../Items"));
        QTest::newRow("all-zero group") << QStringLiteral("GroupId") << QJsonValue(kZero);
        QTest::newRow("31-digit group") << QStringLiteral("GroupId") << QJsonValue(kGroup.left(31));
        QTest::newRow("misplaced dashes")
            << QStringLiteral("GroupId")
            << QJsonValue(QStringLiteral("5f1e2d3c4-b5a-6978-8796-a5b4c3d2e1f0"));
        QTest::newRow("non-hex entry")
            << QStringLiteral("PlaylistItemId") << QJsonValue(QString(32, QLatin1Char('g')));
        QTest::newRow("when with an offset")
            << QStringLiteral("When") << QJsonValue(QStringLiteral("2026-11-16T16:37:59+01:00"));
        QTest::newRow("emitted at missing")
            << QStringLiteral("EmittedAt") << QJsonValue(QJsonValue::Undefined);
    }
    void commandsThatAreDropped()
    {
        QFETCH(QString, key);
        QFETCH(QJsonValue, value);
        QJsonObject data = frameData(QStringLiteral("ws_syncplay_command")).toObject();
        if (value.isUndefined())
            data.remove(key);
        else
            data.insert(key, value);
        QVERIFY(!SyncPlay::parseCommand(data));
    }

    void aSeekNeedsItsPosition()
    {
        QJsonObject data = frameData(QStringLiteral("ws_syncplay_command")).toObject();
        data.insert(QStringLiteral("Command"), QStringLiteral("Seek"));
        data.remove(QStringLiteral("PositionTicks"));
        QVERIFY(!SyncPlay::parseCommand(data));
        data.insert(QStringLiteral("Command"), QStringLiteral("Pause"));
        const auto pause = SyncPlay::parseCommand(data);
        QVERIFY(pause);
        QCOMPARE(pause->positionTicks, qint64(-1));
        QVERIFY(!SyncPlay::parseCommand(QJsonValue(QStringLiteral("Pause"))));
    }

    void groupJoined()
    {
        const auto update =
            SyncPlay::parseGroupUpdate(frameData(QStringLiteral("ws_syncplay_group_joined")));
        QVERIFY(update);
        QCOMPARE(update->type, SyncPlay::GroupUpdate::Type::GroupJoined);
        QCOMPARE(update->groupId, kGroup);
        QCOMPARE(update->group.groupId, kGroup);
        QCOMPARE(update->group.name, QStringLiteral("Movie night"));
        QCOMPARE(update->group.state, SyncPlay::GroupState::Idle);
        QCOMPARE(update->group.participants, QStringList{QStringLiteral("alice")});
        // The group inside must be the one the update names
        QJsonObject inner = frameData(QStringLiteral("ws_syncplay_group_joined"))
                                .toObject()
                                .value(QStringLiteral("Data"))
                                .toObject();
        inner.insert(QStringLiteral("GroupId"), kEntry);
        QVERIFY(!SyncPlay::parseGroupUpdate(
            withPayload(QStringLiteral("ws_syncplay_group_joined"), inner)));
        // A name that cannot be shown leaves the join valid: the session is in the group either way
        inner.insert(QStringLiteral("GroupId"), kGroup);
        inner.insert(QStringLiteral("GroupName"), QStringLiteral("a\nb"));
        inner.insert(QStringLiteral("Participants"),
                     QJsonArray{QStringLiteral("alice"), QStringLiteral("b\nob"), 7});
        const auto badNames = SyncPlay::parseGroupUpdate(
            withPayload(QStringLiteral("ws_syncplay_group_joined"), inner));
        QVERIFY(badNames);
        QVERIFY(badNames->group.name.isEmpty());
        QCOMPARE(badNames->group.participants, QStringList{QStringLiteral("alice")});
    }

    void membersComeAndGo()
    {
        const auto joined =
            SyncPlay::parseGroupUpdate(frameData(QStringLiteral("ws_syncplay_user_joined")));
        QVERIFY(joined);
        QCOMPARE(joined->type, SyncPlay::GroupUpdate::Type::UserJoined);
        QCOMPARE(joined->userName, QStringLiteral("bob"));
        QJsonObject left = frameData(QStringLiteral("ws_syncplay_user_joined")).toObject();
        left.insert(QStringLiteral("Type"), QStringLiteral("UserLeft"));
        QCOMPARE(SyncPlay::parseGroupUpdate(left)->type, SyncPlay::GroupUpdate::Type::UserLeft);
    }

    void namesThatAreNotShown_data()
    {
        QTest::addColumn<QJsonValue>("name");
        // fromUcs4 would drop a byte order mark, so the BMP ones go in as they are
        const auto with = [](char32_t code) {
            const QString character =
                code > 0xffff ? QString::fromUcs4(&code, 1) : QString(QChar(char16_t(code)));
            return QJsonValue(QString(QStringLiteral("bob") + character));
        };
        QTest::newRow("line break") << QJsonValue(QStringLiteral("bob\nalice"));
        QTest::newRow("paragraph separator") << with(0x2029);
        QTest::newRow("right-to-left override") << with(0x202e);
        QTest::newRow("isolate") << with(0x2066);
        QTest::newRow("right-to-left mark") << with(0x200f);
        QTest::newRow("zero-width space") << with(0x200b);
        QTest::newRow("byte order mark") << with(0xfeff);
        QTest::newRow("soft hyphen") << with(0xad);
        QTest::newRow("tag letter") << with(0xe0041);
        QTest::newRow("private use") << with(0xe000);
        QTest::newRow("unassigned") << with(0x0378);
        QTest::newRow("C1 control") << with(0x85);
        QTest::newRow("lone high surrogate")
            << QJsonValue(QString(QStringLiteral("bob") + QChar(0xd800)));
        QTest::newRow("lone low surrogate")
            << QJsonValue(QString(QChar(0xdc00) + QStringLiteral("bob")));
        QTest::newRow("five combining marks")
            << QJsonValue(QString(QStringLiteral("e") + QString(5, QChar(0x0301))));
        QTest::newRow("joiner at the end") << with(0x200d);
        QTest::newRow("joiner at the start")
            << QJsonValue(QString(QChar(0x200c) + QStringLiteral("bob")));
        QTest::newRow("two joiners") << QJsonValue(
            QString(QStringLiteral("bo") + QChar(0x200d) + QChar(0x200c) + QStringLiteral("b")));
        QTest::newRow("blank") << QJsonValue(QStringLiteral("  "));
        QTest::newRow("too long") << QJsonValue(
            QString(SyncPlay::MaxNameChars + 1, QLatin1Char('b')));
        QTest::newRow("not text") << QJsonValue(7);
    }
    void namesThatAreNotShown()
    {
        QFETCH(QJsonValue, name);
        // The member's update still counts, without a name to show
        const auto update = SyncPlay::parseGroupUpdate(
            withPayload(QStringLiteral("ws_syncplay_user_joined"), name));
        QVERIFY(update);
        QCOMPARE(update->type, SyncPlay::GroupUpdate::Type::UserJoined);
        QVERIFY(update->userName.isEmpty());
        QVERIFY(!SyncPlay::isValidRequest(SyncPlay::Route::New,
                                          SyncPlay::newGroupBody(name.toString())));
    }

    void namesThatAreShown()
    {
        const QString longest(SyncPlay::MaxNameChars, QLatin1Char('b'));
        const char32_t emoji = 0x1f37f;
        const char32_t family[] = {0x1f468, 0x200d, 0x1f469, 0x200d, 0x1f467};
        const QString persian = QStringLiteral("می") + QChar(0x200c) + QStringLiteral("خواهم");
        for (const QString &name :
             {longest, QString(QStringLiteral("e") + QString(4, QChar(0x0301))),
              QStringLiteral("Zoë"), QString::fromUcs4(&emoji, 1), QStringLiteral("Ana María"),
              QString::fromUcs4(family, 5), persian}) {
            const auto update = SyncPlay::parseGroupUpdate(
                withPayload(QStringLiteral("ws_syncplay_user_joined"), name));
            QVERIFY(update);
            QCOMPARE(update->userName, name);
        }
    }

    void groupLeftIsMatchedByTheEnvelope()
    {
        const auto update =
            SyncPlay::parseGroupUpdate(frameData(QStringLiteral("ws_syncplay_group_left")));
        QVERIFY(update);
        QCOMPARE(update->type, SyncPlay::GroupUpdate::Type::GroupLeft);
        QCOMPARE(update->groupId, kGroup);
        // Only the refusals may come without a group
        QJsonObject zero = frameData(QStringLiteral("ws_syncplay_group_left")).toObject();
        zero.insert(QStringLiteral("GroupId"), kZero);
        QVERIFY(!SyncPlay::parseGroupUpdate(zero));
        zero.insert(QStringLiteral("Type"), QStringLiteral("UserJoined"));
        QVERIFY(!SyncPlay::parseGroupUpdate(zero));
    }

    void stateUpdate()
    {
        const auto update =
            SyncPlay::parseGroupUpdate(frameData(QStringLiteral("ws_syncplay_state_update")));
        QVERIFY(update);
        QCOMPARE(update->type, SyncPlay::GroupUpdate::Type::StateUpdate);
        QCOMPARE(update->state, SyncPlay::GroupState::Playing);
        QCOMPARE(update->reason, QStringLiteral("Ready"));
        const QJsonObject unknownState{{QStringLiteral("State"), QStringLiteral("Rewinding")},
                                       {QStringLiteral("Reason"), QStringLiteral("Ready")}};
        QVERIFY(!SyncPlay::parseGroupUpdate(
            withPayload(QStringLiteral("ws_syncplay_state_update"), unknownState)));
        const QJsonObject wordyReason{{QStringLiteral("State"), QStringLiteral("Waiting")},
                                      {QStringLiteral("Reason"), QStringLiteral("not letters 1")}};
        QVERIFY(!SyncPlay::parseGroupUpdate(
            withPayload(QStringLiteral("ws_syncplay_state_update"), wordyReason)));
    }

    void playQueue()
    {
        const auto update =
            SyncPlay::parseGroupUpdate(frameData(QStringLiteral("ws_syncplay_play_queue")));
        QVERIFY(update);
        QCOMPARE(update->type, SyncPlay::GroupUpdate::Type::PlayQueue);
        const SyncPlay::PlayQueue &queue = update->queue;
        QCOMPARE(queue.reason, QStringLiteral("NewPlaylist"));
        QCOMPARE(queue.entries.size(), 1);
        QCOMPARE(queue.entries.first().itemId, kItem);
        QCOMPARE(queue.entries.first().playlistItemId, kEntry);
        QCOMPARE(queue.playingIndex, 0);
        QCOMPARE(queue.startPositionTicks, qint64(0));
        QVERIFY(!queue.isPlaying);
        QCOMPARE(queue.shuffleMode, QStringLiteral("Sorted"));
        QCOMPARE(queue.repeatMode, QStringLiteral("RepeatNone"));
    }

    void queuesThatAreDropped_data()
    {
        QTest::addColumn<QString>("key");
        QTest::addColumn<QJsonValue>("value");
        QTest::newRow("index past the end") << QStringLiteral("PlayingItemIndex") << QJsonValue(1);
        QTest::newRow("index below -1") << QStringLiteral("PlayingItemIndex") << QJsonValue(-2);
        QTest::newRow("unknown shuffle")
            << QStringLiteral("ShuffleMode") << QJsonValue(QStringLiteral("Random"));
        QTest::newRow("unknown repeat")
            << QStringLiteral("RepeatMode") << QJsonValue(QStringLiteral("RepeatTwice"));
        QTest::newRow("playing as text")
            << QStringLiteral("IsPlaying") << QJsonValue(QStringLiteral("false"));
        QTest::newRow("bad last update")
            << QStringLiteral("LastUpdate") << QJsonValue(QStringLiteral("yesterday"));
        QTest::newRow("entry without an id")
            << QStringLiteral("Playlist")
            << QJsonValue(QJsonArray{QJsonObject{{QStringLiteral("ItemId"), kItem}}});
    }
    void queuesThatAreDropped()
    {
        QFETCH(QString, key);
        QFETCH(QJsonValue, value);
        const QJsonObject queue = withValue(frameData(QStringLiteral("ws_syncplay_play_queue"))
                                                .toObject()
                                                .value(QStringLiteral("Data"))
                                                .toObject(),
                                            key, value);
        QVERIFY(!SyncPlay::parseGroupUpdate(
            withPayload(QStringLiteral("ws_syncplay_play_queue"), queue)));
    }

    void queueBounds()
    {
        QJsonObject queue = frameData(QStringLiteral("ws_syncplay_play_queue"))
                                .toObject()
                                .value(QStringLiteral("Data"))
                                .toObject();
        // An empty queue has nothing current
        queue.insert(QStringLiteral("Playlist"), QJsonArray());
        queue.insert(QStringLiteral("PlayingItemIndex"), -1);
        const auto empty = SyncPlay::parseGroupUpdate(
            withPayload(QStringLiteral("ws_syncplay_play_queue"), queue));
        QVERIFY(empty);
        QCOMPARE(empty->queue.playingIndex, -1);
        QJsonArray full;
        for (int i = 0; i <= SyncPlay::MaxQueueEntries; ++i)
            full.append(QJsonObject{{QStringLiteral("ItemId"), kItem},
                                    {QStringLiteral("PlaylistItemId"), kEntry}});
        queue.insert(QStringLiteral("Playlist"), full);
        QVERIFY(!SyncPlay::parseGroupUpdate(
            withPayload(QStringLiteral("ws_syncplay_play_queue"), queue)));
        full.removeLast();
        queue.insert(QStringLiteral("Playlist"), full);
        QVERIFY(SyncPlay::parseGroupUpdate(
            withPayload(QStringLiteral("ws_syncplay_play_queue"), queue)));
    }

    void refusalsComeWithoutAGroup()
    {
        const auto update =
            SyncPlay::parseGroupUpdate(frameData(QStringLiteral("ws_syncplay_not_in_group")));
        QVERIFY(update);
        QCOMPARE(update->type, SyncPlay::GroupUpdate::Type::NotInGroup);
        QVERIFY(update->groupId.isEmpty());
        const QList<std::pair<QString, SyncPlay::GroupUpdate::Type>> others{
            {QStringLiteral("GroupDoesNotExist"), SyncPlay::GroupUpdate::Type::GroupDoesNotExist},
            {QStringLiteral("CreateGroupDenied"), SyncPlay::GroupUpdate::Type::CreateGroupDenied},
            {QStringLiteral("JoinGroupDenied"), SyncPlay::GroupUpdate::Type::JoinGroupDenied},
            {QStringLiteral("LibraryAccessDenied"),
             SyncPlay::GroupUpdate::Type::LibraryAccessDenied}};
        for (const auto &[type, expected] : others) {
            QJsonObject data = frameData(QStringLiteral("ws_syncplay_not_in_group")).toObject();
            data.insert(QStringLiteral("Type"), type);
            QCOMPARE(SyncPlay::parseGroupUpdate(data)->type, expected);
        }
        QJsonObject unknown = frameData(QStringLiteral("ws_syncplay_not_in_group")).toObject();
        unknown.insert(QStringLiteral("Type"), QStringLiteral("GroupExploded"));
        QVERIFY(!SyncPlay::parseGroupUpdate(unknown));
        unknown.insert(QStringLiteral("GroupId"), QStringLiteral("../Items"));
        unknown.insert(QStringLiteral("Type"), QStringLiteral("NotInGroup"));
        QVERIFY(!SyncPlay::parseGroupUpdate(unknown));
    }

    void groupList()
    {
        const auto groups = SyncPlay::parseGroupList(
            QJsonDocument::fromJson(fixture(QStringLiteral("syncplay_group_list"))).array());
        QVERIFY(groups);
        QCOMPARE(groups->size(), 1);
        QCOMPARE(groups->first().state, SyncPlay::GroupState::Playing);
        QCOMPARE(groups->first().participants,
                 (QStringList{QStringLiteral("alice"), QStringLiteral("bob")}));
        QVERIFY(!SyncPlay::parseGroupList(QJsonObject()));
    }

    void invalidGroupsAreLeftOut()
    {
        const QJsonObject good = fixtureObject(QStringLiteral("syncplay_group"));
        QJsonArray crowd;
        for (int i = 0; i <= SyncPlay::MaxParticipants; ++i)
            crowd.append(QStringLiteral("member%1").arg(i));
        const QJsonArray list{
            withValue(good, QStringLiteral("GroupName"), QStringLiteral("a\nb")),
            good,
            withValue(good, QStringLiteral("Participants"), crowd),
            withValue(good, QStringLiteral("State"), QStringLiteral("idle")),
            withValue(good, QStringLiteral("GroupId"), kZero),
            withValue(good, QStringLiteral("LastUpdatedAt"), QStringLiteral("now")),
            QStringLiteral("not a group")};
        const auto groups = SyncPlay::parseGroupList(list);
        QVERIFY(groups);
        // Only what identifies a group drops it; a name that cannot be shown is left empty and the members are cut
        QCOMPARE(groups->size(), 3);
        QVERIFY(groups->at(0).name.isEmpty());
        QCOMPARE(groups->at(1).name, QStringLiteral("Movie night"));
        QCOMPARE(groups->at(2).participants.size(), SyncPlay::MaxParticipants);
        QCOMPARE(groups->at(2).participants.last(),
                 QStringLiteral("member%1").arg(SyncPlay::MaxParticipants - 1));
        QJsonArray many;
        for (int i = 0; i <= SyncPlay::MaxGroups; ++i)
            many.append(good);
        QCOMPARE(SyncPlay::parseGroupList(many)->size(), SyncPlay::MaxGroups);
    }

    void utcTime()
    {
        const auto time = SyncPlay::parseUtcTime(fixtureObject(QStringLiteral("get_utc_time")));
        QVERIFY(time);
        QCOMPARE(time->receptionMs, utcMs(2026, 11, 16, 16, 37, 39, 236));
        QCOMPARE(time->transmissionMs, time->receptionMs);
        const QJsonObject backwards{
            {QStringLiteral("RequestReceptionTime"), QStringLiteral("2026-11-16T16:37:39.5Z")},
            {QStringLiteral("ResponseTransmissionTime"), QStringLiteral("2026-11-16T16:37:39.4Z")}};
        QVERIFY(!SyncPlay::parseUtcTime(backwards));
    }

    void bodiesMatchTheGoldens()
    {
        const QJsonObject ready =
            SyncPlay::readinessBody(utcMs(2026, 11, 16, 16, 37, 57, 317), 600000000, false, kEntry);
        QCOMPARE(ready, fixtureObject(QStringLiteral("syncplay_ready_request")));
        QVERIFY(SyncPlay::isValidRequest(SyncPlay::Route::Ready, ready));
        QVERIFY(SyncPlay::isValidRequest(SyncPlay::Route::Buffering, ready));
        const QJsonObject queue = SyncPlay::newQueueBody({kItem}, 0, 0);
        QCOMPARE(queue, fixtureObject(QStringLiteral("syncplay_new_queue_request")));
        QVERIFY(SyncPlay::isValidRequest(SyncPlay::Route::SetNewQueue, queue));
    }

    void validRequests()
    {
        using SyncPlay::Route;
        QVERIFY(SyncPlay::isValidRequest(Route::New,
                                         SyncPlay::newGroupBody(QStringLiteral("Movie night"))));
        QVERIFY(SyncPlay::isValidRequest(Route::Join, SyncPlay::joinBody(kGroup)));
        for (const Route route : {Route::Leave, Route::Pause, Route::Unpause, Route::Stop})
            QVERIFY(SyncPlay::isValidRequest(route, {}));
        QVERIFY(
            SyncPlay::isValidRequest(Route::Seek, SyncPlay::seekBody(SyncPlay::MaxPositionTicks)));
        QVERIFY(SyncPlay::isValidRequest(Route::Ping, SyncPlay::pingBody(SyncPlay::MaxPingMs)));
        QVERIFY(SyncPlay::isValidRequest(Route::SetIgnoreWait, SyncPlay::ignoreWaitBody(true)));
        QVERIFY(SyncPlay::isValidRequest(Route::NextItem, SyncPlay::playlistItemBody(kEntry)));
        QVERIFY(SyncPlay::isValidRequest(Route::PreviousItem, SyncPlay::playlistItemBody(kEntry)));
        QVERIFY(
            SyncPlay::isValidRequest(Route::SetPlaylistItem, SyncPlay::playlistItemBody(kEntry)));
        QVERIFY(SyncPlay::isValidRequest(Route::Queue, SyncPlay::queueBody({kItem}, true)));
        QCOMPARE(SyncPlay::queueBody({kItem}, true).value(QStringLiteral("Mode")).toString(),
                 QStringLiteral("QueueNext"));
        QCOMPARE(SyncPlay::queueBody({kItem}, false).value(QStringLiteral("Mode")).toString(),
                 QStringLiteral("Queue"));
        QVERIFY(SyncPlay::isValidRequest(Route::RemoveFromPlaylist,
                                         SyncPlay::removeBody({kEntry}, false, false)));
        // Clearing the queue names no entry
        QVERIFY(SyncPlay::isValidRequest(Route::RemoveFromPlaylist,
                                         SyncPlay::removeBody({}, true, true)));
        QVERIFY(SyncPlay::isValidRequest(Route::MovePlaylistItem, SyncPlay::moveBody(kEntry, 0)));
        for (const char *mode : {"RepeatNone", "RepeatAll", "RepeatOne"})
            QVERIFY(SyncPlay::isValidRequest(Route::SetRepeatMode,
                                             SyncPlay::repeatBody(QString::fromLatin1(mode))));
        QCOMPARE(SyncPlay::shuffleBody(true),
                 (QJsonObject{{QStringLiteral("Mode"), QStringLiteral("Shuffle")}}));
        QCOMPARE(SyncPlay::shuffleBody(false),
                 (QJsonObject{{QStringLiteral("Mode"), QStringLiteral("Sorted")}}));
        QVERIFY(SyncPlay::isValidRequest(Route::SetShuffleMode, SyncPlay::shuffleBody(true)));
        QStringList longest;
        for (int i = 0; i < SyncPlay::MaxNewQueueItems; ++i)
            longest.append(kItem);
        QVERIFY(SyncPlay::isValidRequest(
            Route::SetNewQueue,
            SyncPlay::newQueueBody(longest, SyncPlay::MaxNewQueueItems - 1, 0)));
    }

    void refusedRequests()
    {
        using SyncPlay::Route;
        QVERIFY(!SyncPlay::isValidRequest(Route::New, SyncPlay::newGroupBody(QString())));
        QVERIFY(
            !SyncPlay::isValidRequest(Route::New, SyncPlay::newGroupBody(QStringLiteral("   "))));
        QVERIFY(
            !SyncPlay::isValidRequest(Route::New, SyncPlay::newGroupBody(QStringLiteral("a\nb"))));
        QVERIFY(!SyncPlay::isValidRequest(
            Route::New,
            SyncPlay::newGroupBody(QString(SyncPlay::MaxNameChars + 1, QLatin1Char('a')))));
        QVERIFY(
            !SyncPlay::isValidRequest(Route::Join, SyncPlay::joinBody(QStringLiteral("../Items"))));
        QVERIFY(!SyncPlay::isValidRequest(Route::Join, SyncPlay::joinBody(kZero)));
        QVERIFY(!SyncPlay::isValidRequest(Route::Leave, SyncPlay::seekBody(0)));
        QVERIFY(!SyncPlay::isValidRequest(Route::Seek, SyncPlay::seekBody(-1)));
        QVERIFY(!SyncPlay::isValidRequest(Route::Seek,
                                          SyncPlay::seekBody(SyncPlay::MaxPositionTicks + 1)));
        QVERIFY(
            !SyncPlay::isValidRequest(Route::Ready, SyncPlay::readinessBody(0, 0, false, kZero)));
        QVERIFY(
            !SyncPlay::isValidRequest(Route::Ping, SyncPlay::pingBody(SyncPlay::MaxPingMs + 1)));
        QVERIFY(!SyncPlay::isValidRequest(Route::Ping, SyncPlay::pingBody(-1)));
        QVERIFY(!SyncPlay::isValidRequest(Route::SetNewQueue, SyncPlay::newQueueBody({}, 0, 0)));
        QVERIFY(
            !SyncPlay::isValidRequest(Route::SetNewQueue, SyncPlay::newQueueBody({kItem}, 1, 0)));
        QVERIFY(!SyncPlay::isValidRequest(
            Route::SetNewQueue, SyncPlay::newQueueBody({QStringLiteral("../Items")}, 0, 0)));
        QStringList tooMany;
        for (int i = 0; i <= SyncPlay::MaxNewQueueItems; ++i)
            tooMany.append(kItem);
        QVERIFY(
            !SyncPlay::isValidRequest(Route::SetNewQueue, SyncPlay::newQueueBody(tooMany, 0, 0)));
        // A key the route does not take is refused, as is a missing one
        QVERIFY(!SyncPlay::isValidRequest(
            Route::Join, withValue(SyncPlay::joinBody(kGroup), QStringLiteral("Extra"), true)));
        QVERIFY(!SyncPlay::isValidRequest(Route::NextItem, {}));
        QVERIFY(!SyncPlay::isValidRequest(Route::Queue, SyncPlay::queueBody({}, false)));
        QVERIFY(!SyncPlay::isValidRequest(
            Route::Queue, withValue(SyncPlay::queueBody({kItem}, false), QStringLiteral("Mode"),
                                    QStringLiteral("Last"))));
        QVERIFY(!SyncPlay::isValidRequest(Route::Queue, SyncPlay::queueBody(tooMany, false)));
        QVERIFY(!SyncPlay::isValidRequest(
            Route::Queue, SyncPlay::queueBody({QStringLiteral("../Items")}, false)));
        // An entry to remove must be named unless the queue is cleared
        QVERIFY(!SyncPlay::isValidRequest(Route::RemoveFromPlaylist,
                                          SyncPlay::removeBody({}, false, false)));
        QVERIFY(!SyncPlay::isValidRequest(Route::RemoveFromPlaylist,
                                          SyncPlay::removeBody({kZero}, false, false)));
        QVERIFY(!SyncPlay::isValidRequest(Route::MovePlaylistItem, SyncPlay::moveBody(kEntry, -1)));
        QVERIFY(!SyncPlay::isValidRequest(Route::MovePlaylistItem,
                                          SyncPlay::moveBody(kEntry, SyncPlay::MaxQueueEntries)));
        QVERIFY(!SyncPlay::isValidRequest(Route::SetRepeatMode,
                                          SyncPlay::repeatBody(QStringLiteral("RepeatTwice"))));
        QVERIFY(!SyncPlay::isValidRequest(Route::SetShuffleMode,
                                          SyncPlay::repeatBody(QStringLiteral("Random"))));
        QVERIFY(
            !SyncPlay::isValidRequest(Route::SetPlaylistItem, SyncPlay::playlistItemBody(kZero)));
    }

    void routeNames()
    {
        using SyncPlay::Route;
        const QList<std::pair<Route, QString>> names{
            {Route::New, QStringLiteral("New")},
            {Route::Join, QStringLiteral("Join")},
            {Route::Leave, QStringLiteral("Leave")},
            {Route::Pause, QStringLiteral("Pause")},
            {Route::Unpause, QStringLiteral("Unpause")},
            {Route::Stop, QStringLiteral("Stop")},
            {Route::Seek, QStringLiteral("Seek")},
            {Route::Buffering, QStringLiteral("Buffering")},
            {Route::Ready, QStringLiteral("Ready")},
            {Route::Ping, QStringLiteral("Ping")},
            {Route::SetIgnoreWait, QStringLiteral("SetIgnoreWait")},
            {Route::SetNewQueue, QStringLiteral("SetNewQueue")},
            {Route::NextItem, QStringLiteral("NextItem")},
            {Route::PreviousItem, QStringLiteral("PreviousItem")},
            {Route::SetPlaylistItem, QStringLiteral("SetPlaylistItem")},
            {Route::Queue, QStringLiteral("Queue")},
            {Route::RemoveFromPlaylist, QStringLiteral("RemoveFromPlaylist")},
            {Route::MovePlaylistItem, QStringLiteral("MovePlaylistItem")},
            {Route::SetRepeatMode, QStringLiteral("SetRepeatMode")},
            {Route::SetShuffleMode, QStringLiteral("SetShuffleMode")}};
        for (const auto &[route, name] : names)
            QCOMPARE(SyncPlay::routeName(route), name);
    }

    void clientListsTheGroups()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(!client.fetchSyncPlayGroups());
        QVERIFY(login(client, server));
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::syncPlayGroupsReady);
        QSignalSpy failed(&client, &Kinefin::JellyfinClient::syncPlayGroupsFailed);
        QVERIFY(client.fetchSyncPlayGroups());
        QTRY_COMPARE(ready.size(), 1);
        const auto groups = ready.first().first().value<QList<SyncPlay::GroupInfo>>();
        QCOMPARE(groups.size(), 1);
        QCOMPARE(groups.first().name, QStringLiteral("Movie night"));
        QCOMPARE(server.syncPlayListRequests, 1);

        server.syncPlayListBody = "{\"not\":\"a list\"}";
        QVERIFY(client.fetchSyncPlayGroups());
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(failed.first().first().toInt(), 200);
        QCOMPARE(ready.size(), 1);
    }

    void aNewerListSupersedesThePendingOne()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::syncPlayGroupsReady);
        QSignalSpy failed(&client, &Kinefin::JellyfinClient::syncPlayGroupsFailed);
        QVERIFY(client.fetchSyncPlayGroups());
        QVERIFY(client.fetchSyncPlayGroups());
        QTRY_COMPARE(ready.size(), 1);
        QTest::qWait(100);
        QCOMPARE(ready.size(), 1);
        QCOMPARE(failed.size(), 0);
    }

    void clientPostsGroupRequests()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(!client.postSyncPlay(SyncPlay::Route::Leave, {}));
        QVERIFY(login(client, server));
        QSignalSpy finished(&client, &Kinefin::JellyfinClient::syncPlayRequestFinished);
        QVERIFY(client.postSyncPlay(SyncPlay::Route::Join, SyncPlay::joinBody(kGroup)));
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(0).value<SyncPlay::Route>(), SyncPlay::Route::Join);
        QVERIFY(finished.first().at(1).toBool());
        QCOMPARE(finished.first().at(2).toInt(), 204);
        QCOMPARE(server.syncPlayRequests.size(), 1);
        const auto &join = server.syncPlayRequests.first();
        QCOMPARE(join.route, QStringLiteral("Join"));
        QCOMPARE(join.body, SyncPlay::joinBody(kGroup));
        QCOMPARE(join.contentType, QByteArray("application/json"));
        QVERIFY(join.authorization.contains(kTokenField));

        // New answers 200 with the group: delivered all the same
        server.syncPlayStatus = 200;
        QVERIFY(client.postSyncPlay(SyncPlay::Route::New,
                                    SyncPlay::newGroupBody(QStringLiteral("Movie night"))));
        QTRY_COMPARE(finished.size(), 2);
        QVERIFY(finished.last().at(1).toBool());
        QCOMPARE(server.syncPlayRequests.last().route, QStringLiteral("New"));

        // An empty route sends an empty object
        server.syncPlayStatus = 204;
        QVERIFY(client.postSyncPlay(SyncPlay::Route::Leave, {}));
        QTRY_COMPARE(finished.size(), 3);
        QCOMPARE(server.syncPlayRequests.last().route, QStringLiteral("Leave"));
        QVERIFY(server.syncPlayRequests.last().body.isEmpty());

        server.syncPlayStatus = 500;
        QVERIFY(client.postSyncPlay(SyncPlay::Route::Pause, {}));
        QTRY_COMPARE(finished.size(), 4);
        QVERIFY(!finished.last().at(1).toBool());
        QCOMPARE(finished.last().at(2).toInt(), 500);
    }

    void clientRefusesABodyThatDoesNotFit()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QVERIFY(!client.postSyncPlay(SyncPlay::Route::Join,
                                     SyncPlay::joinBody(QStringLiteral("../Items"))));
        QVERIFY(!client.postSyncPlay(SyncPlay::Route::Seek, {}));
        QVERIFY(!client.postSyncPlay(SyncPlay::Route::Leave, SyncPlay::seekBody(0)));
        QTest::qWait(50);
        QVERIFY(server.syncPlayRequests.isEmpty());
    }

    void unauthorizedEndsTheSession()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy expired(&client, &Kinefin::JellyfinClient::sessionExpired);
        QSignalSpy finished(&client, &Kinefin::JellyfinClient::syncPlayRequestFinished);
        server.syncPlayStatus = 401;
        QVERIFY(client.postSyncPlay(SyncPlay::Route::Unpause, {}));
        // The one waiting behind it is never sent
        QVERIFY(client.postSyncPlay(SyncPlay::Route::Pause, {}));
        QTRY_COMPARE(expired.size(), 1);
        QVERIFY(!client.isAuthenticated());
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(!finished.first().at(1).toBool());
        QTest::qWait(100);
        QCOMPARE(finished.size(), 1);
        QCOMPARE(server.syncPlayRequests.size(), 1);
        QVERIFY(!client.postSyncPlay(SyncPlay::Route::Unpause, {}));
    }

    void clientReadsTheServerTime()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(!client.fetchUtcTime());
        QVERIFY(login(client, server));
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::utcTimeReady);
        QSignalSpy failed(&client, &Kinefin::JellyfinClient::utcTimeFailed);
        QVERIFY(client.fetchUtcTime());
        QTRY_COMPARE(ready.size(), 1);
        QCOMPARE(ready.first().at(0).toLongLong(), utcMs(2026, 11, 16, 16, 37, 39, 236));
        QCOMPARE(server.utcTimeRequests, 1);
        server.utcTimeBody = "{\"RequestReceptionTime\":\"now\"}";
        QVERIFY(client.fetchUtcTime());
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(failed.first().first().toInt(), 200);
        server.utcTimeStatus = 503;
        QVERIFY(client.fetchUtcTime());
        QTRY_COMPARE(failed.size(), 2);
        QCOMPARE(failed.last().first().toInt(), 503);
    }

    void requestsGoOutOneAtATimeInOrder()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy finished(&client, &Kinefin::JellyfinClient::syncPlayRequestFinished);
        server.holdSyncPlay = true;
        const QJsonObject readiness = SyncPlay::readinessBody(0, 0, true, kEntry);
        QVERIFY(client.postSyncPlay(SyncPlay::Route::Buffering, readiness));
        QVERIFY(client.postSyncPlay(SyncPlay::Route::Ready, readiness));
        QVERIFY(client.postSyncPlay(SyncPlay::Route::Pause, {}));
        const QStringList expected{QStringLiteral("Buffering"), QStringLiteral("Ready"),
                                   QStringLiteral("Pause")};
        for (int sent = 1; sent <= 3; ++sent) {
            QTRY_COMPARE(server.heldCount(), 1);
            QTest::qWait(50);
            QCOMPARE(server.syncPlayRequests.size(), sent);
            QCOMPARE(server.syncPlayRequests.last().route, expected.at(sent - 1));
            server.releaseHeld();
            QTRY_COMPARE(finished.size(), sent);
            QCOMPARE(finished.last().at(0).value<SyncPlay::Route>(),
                     sent == 1 ? SyncPlay::Route::Buffering
                               : (sent == 2 ? SyncPlay::Route::Ready : SyncPlay::Route::Pause));
        }
    }

    void theQueueIsBounded()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        server.holdSyncPlay = true;
        int queued = 0;
        while (client.postSyncPlay(SyncPlay::Route::Ping, SyncPlay::pingBody(10)) && queued < 100)
            ++queued;
        QCOMPARE(queued, Kinefin::JellyfinClient::MaxQueuedSyncPlayRequests);
        QTRY_COMPARE(server.heldCount(), 1);
        server.holdSyncPlay = false;
        server.releaseHeld();
        QTRY_COMPARE(server.syncPlayRequests.size(), queued);
        QVERIFY(client.postSyncPlay(SyncPlay::Route::Ping, SyncPlay::pingBody(10)));
    }

    void aSessionEndDropsWhatIsPending()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy finished(&client, &Kinefin::JellyfinClient::syncPlayRequestFinished);
        QSignalSpy groups(&client, &Kinefin::JellyfinClient::syncPlayGroupsReady);
        QSignalSpy groupsFailed(&client, &Kinefin::JellyfinClient::syncPlayGroupsFailed);
        QSignalSpy time(&client, &Kinefin::JellyfinClient::utcTimeReady);
        QSignalSpy timeFailed(&client, &Kinefin::JellyfinClient::utcTimeFailed);
        server.holdSyncPlay = true;
        QVERIFY(client.postSyncPlay(SyncPlay::Route::Leave, {}));
        QVERIFY(client.postSyncPlay(SyncPlay::Route::Leave, {}));
        QTRY_COMPARE(server.heldCount(), 1);
        QVERIFY(client.fetchSyncPlayGroups());
        QVERIFY(client.fetchUtcTime());
        client.disconnectFromServer();
        server.releaseHeld();
        QTest::qWait(150);
        QCOMPARE(finished.size(), 0);
        QCOMPARE(server.syncPlayRequests.size(), 1);
        QCOMPARE(groups.size() + groupsFailed.size() + time.size() + timeFailed.size(), 0);
    }

    void unauthorizedReadsEndTheSession()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy expired(&client, &Kinefin::JellyfinClient::sessionExpired);
        QSignalSpy groupsFailed(&client, &Kinefin::JellyfinClient::syncPlayGroupsFailed);
        server.syncPlayListStatus = 401;
        QVERIFY(client.fetchSyncPlayGroups());
        QTRY_COMPARE(groupsFailed.size(), 1);
        QCOMPARE(groupsFailed.first().first().toInt(), 401);
        QCOMPARE(expired.size(), 1);

        Kinefin::JellyfinClient second;
        QVERIFY(login(second, server));
        QSignalSpy secondExpired(&second, &Kinefin::JellyfinClient::sessionExpired);
        QSignalSpy timeFailed(&second, &Kinefin::JellyfinClient::utcTimeFailed);
        server.utcTimeStatus = 401;
        QVERIFY(second.fetchUtcTime());
        QTRY_COMPARE(timeFailed.size(), 1);
        QCOMPARE(timeFailed.first().first().toInt(), 401);
        QCOMPARE(secondExpired.size(), 1);
    }

    void aNewerTimeSupersedesThePendingOne()
    {
        FakeJellyfin server;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(login(client, server));
        QSignalSpy ready(&client, &Kinefin::JellyfinClient::utcTimeReady);
        QSignalSpy failed(&client, &Kinefin::JellyfinClient::utcTimeFailed);
        QVERIFY(client.fetchUtcTime());
        QVERIFY(client.fetchUtcTime());
        QTRY_COMPARE(ready.size(), 1);
        QTest::qWait(100);
        QCOMPARE(ready.size(), 1);
        QCOMPARE(failed.size(), 0);
    }

    void theClientKnowsTheUsersPolicy_data()
    {
        QTest::addColumn<QString>("sent");
        QTest::addColumn<QString>("read");
        QTest::newRow("create and join")
            << QStringLiteral("CreateAndJoinGroups") << QStringLiteral("CreateAndJoinGroups");
        QTest::newRow("join only") << QStringLiteral("JoinGroups") << QStringLiteral("JoinGroups");
        QTest::newRow("none") << QStringLiteral("None") << QStringLiteral("None");
        QTest::newRow("unknown value") << QStringLiteral("Everything") << QString();
        QTest::newRow("absent") << QString() << QString();
    }
    void theClientKnowsTheUsersPolicy()
    {
        QFETCH(QString, sent);
        QFETCH(QString, read);
        FakeJellyfin server;
        server.syncPlayAccess = sent;
        QVERIFY(server.listen());
        Kinefin::JellyfinClient client;
        QVERIFY(client.syncPlayAccess().isEmpty());
        QVERIFY(login(client, server));
        QCOMPARE(client.syncPlayAccess(), read);
        // A later /Users/Me brings the policy again
        server.syncPlayAccess = QStringLiteral("None");
        QSignalSpy refreshed(&client, &Kinefin::JellyfinClient::userProfileRefreshed);
        QVERIFY(client.verifySession());
        QTRY_COMPARE(refreshed.size(), 1);
        QCOMPARE(client.syncPlayAccess(), QStringLiteral("None"));
        client.disconnectFromServer();
        QVERIFY(client.syncPlayAccess().isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestSyncPlayProtocol)
#include "test_syncplay_protocol.moc"
