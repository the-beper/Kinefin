// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>

#include <optional>

// The one owner of the SyncPlay wire format (Jellyfin 12.4, observed live): the frames and replies the server sends, parsed strictly (a value outside the rules drops the whole frame or entry), and the request bodies the client sends, checked by one validator before they leave
namespace Kinefin::SyncPlay {

// The bounds of what the server may send: entries of a group queue, groups in a list, members of a group, characters of a group or user name, and 24 h in ticks for any position
constexpr int MaxQueueEntries = 10000;
constexpr int MaxGroups = 100;
constexpr int MaxParticipants = 64;
constexpr int MaxNameChars = 128;
constexpr qint64 MaxPositionTicks = 24LL * 3600 * 10'000'000;
// The most items a new group queue may carry, and the largest round trip a Ping may report (ms)
constexpr int MaxNewQueueItems = 500;
constexpr int MaxPingMs = 60'000;

enum class GroupState { Idle, Waiting, Paused, Playing };

// A group as `GET /SyncPlay/List`, `POST /SyncPlay/New` and `GroupJoined` describe it. `participants` holds user names (one per user, not per session), at most MaxParticipants; the names are remote text for plain-text labels only, and one that breaks the rules of a shown name is left out (a member) or empty (the group's)
struct GroupInfo {
    QString groupId;
    QString name;
    GroupState state = GroupState::Idle;
    QStringList participants;
    qint64 lastUpdatedMs = 0;
};

struct QueueEntry {
    QString itemId;
    QString playlistItemId;
};

// The group's queue (`PlayQueue` update). `playingIndex` is -1 when nothing is current; `shuffleMode` is "Sorted" or "Shuffle", `repeatMode` "RepeatNone", "RepeatAll" or "RepeatOne"
struct PlayQueue {
    QString reason;
    qint64 lastUpdateMs = 0;
    QList<QueueEntry> entries;
    int playingIndex = -1;
    qint64 startPositionTicks = 0;
    bool isPlaying = false;
    QString shuffleMode;
    QString repeatMode;
};

// A `SyncPlayCommand` frame: what to do and when, in server time. Every id is in the 32-digit lower-case form; `playlistItemId` is empty when the server sent the all-zero id (the `Stop` that follows every join), `positionTicks` -1 when the frame carried none (a Seek always carries one)
struct Command {
    enum class Kind { Unpause, Pause, Seek, Stop };
    Kind kind = Kind::Stop;
    QString groupId;
    QString playlistItemId;
    qint64 whenMs = 0;
    qint64 emittedAtMs = 0;
    qint64 positionTicks = -1;
};

// A `SyncPlayGroupUpdate` frame; only the members of its type are filled (`userName` is empty when the name breaks the rules of a shown name). `groupId` is empty for the refusals (the server sends the all-zero id with them); `GroupLeft` is matched by it, never by its `Data` (the dashed form of the same id)
struct GroupUpdate {
    enum class Type {
        GroupJoined,
        UserJoined,
        UserLeft,
        GroupLeft,
        StateUpdate,
        PlayQueue,
        NotInGroup,
        GroupDoesNotExist,
        CreateGroupDenied,
        JoinGroupDenied,
        LibraryAccessDenied
    };
    Type type = Type::NotInGroup;
    QString groupId;
    GroupInfo group;
    QString userName;
    GroupState state = GroupState::Idle;
    QString reason;
    SyncPlay::PlayQueue queue;
};

// `GET /GetUtcTime`, in server ms since the epoch
struct UtcTime {
    qint64 receptionMs = 0;
    qint64 transmissionMs = 0;
};

// The requests of the closed set `JellyfinClient::postSyncPlay` sends, each to `POST /SyncPlay/<name>`
enum class Route {
    New,
    Join,
    Leave,
    Pause,
    Unpause,
    Stop,
    Seek,
    Buffering,
    Ready,
    Ping,
    SetIgnoreWait,
    SetNewQueue,
    NextItem,
    PreviousItem,
    SetPlaylistItem,
    Queue,
    RemoveFromPlaylist,
    MovePlaylistItem,
    SetRepeatMode,
    SetShuffleMode
};

// An ISO 8601 UTC time with `Z` and 0 to 7 fraction digits (the server leaves out trailing zeros), as ms since the epoch; nullopt for anything else
std::optional<qint64> parseServerDateMs(QStringView text);
// The same instant as the server writes it, with 7 fraction digits
QString serverDate(qint64 ms);

std::optional<Command> parseCommand(const QJsonValue &data);
std::optional<GroupUpdate> parseGroupUpdate(const QJsonValue &data);
std::optional<GroupInfo> parseGroupInfo(const QJsonValue &value);
// The valid groups of a list (an invalid entry is left out, the list cut at MaxGroups); nullopt when the reply is not an array
std::optional<QList<GroupInfo>> parseGroupList(const QJsonValue &value);
std::optional<UtcTime> parseUtcTime(const QJsonValue &value);

// The path after `/SyncPlay/`
QString routeName(Route route);
// The body builders; their result still goes through isValidRequest, which refuses what does not fit the route (an id that is not a GUID, a position out of range, an empty or overlong name or queue)
QJsonObject newGroupBody(const QString &name);
QJsonObject joinBody(const QString &groupId);
QJsonObject seekBody(qint64 positionTicks);
QJsonObject readinessBody(qint64 whenMs, qint64 positionTicks, bool isPlaying,
                          const QString &playlistItemId);
QJsonObject pingBody(qint64 pingMs);
QJsonObject ignoreWaitBody(bool ignoreWait);
QJsonObject newQueueBody(const QStringList &itemIds, int playingPosition,
                         qint64 startPositionTicks);
// NextItem, PreviousItem and SetPlaylistItem
QJsonObject playlistItemBody(const QString &playlistItemId);
// `next` puts the items after the playing entry (QueueNext), else at the end (Queue)
QJsonObject queueBody(const QStringList &itemIds, bool next);
// Removes entries; `clearPlaylist` empties the whole queue (the ids may then be none) and `clearPlayingItem` lets the playing entry go too
QJsonObject removeBody(const QStringList &playlistItemIds, bool clearPlaylist,
                       bool clearPlayingItem);
// `newIndex` is the entry's index after the move (observed)
QJsonObject moveBody(const QString &playlistItemId, int newIndex);
// "RepeatNone", "RepeatAll" or "RepeatOne"
QJsonObject repeatBody(const QString &mode);
QJsonObject shuffleBody(bool shuffle);
bool isValidRequest(Route route, const QJsonObject &body);

} // namespace Kinefin::SyncPlay

Q_DECLARE_METATYPE(Kinefin::SyncPlay::GroupInfo)
Q_DECLARE_METATYPE(Kinefin::SyncPlay::Command)
Q_DECLARE_METATYPE(Kinefin::SyncPlay::GroupUpdate)
Q_DECLARE_METATYPE(Kinefin::SyncPlay::Route)
