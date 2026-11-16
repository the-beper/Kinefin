// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/SyncPlayProtocol.h"
#include "player/GroupPlayback.h"
#include "syncplay/ServerClock.h"
#include "syncplay/SyncPlayGroupsModel.h"
#include "syncplay/SyncPlayQueueModel.h"

#include <QList>
#include <QObject>
#include <QStringList>
#include <QTimer>

#include <optional>
#include <utility>

namespace Kinefin {
class JellyfinClient;
class JellyfinSocketClient;
class PlayerController;

// Watching together through the server's SyncPlay groups (the wire format is SyncPlayProtocol.h). It creates, joins and leaves a group, keeps the server's clock, opens the group's playing item paused, tells the server Ready once that item (or a seek in it) has settled, carries out the group's pause, unpause and seek at server time and its stop at once, keeps the playback in step (a slight speed change, a skip for a large drift), reports a cache stall as Buffering, asks for the next entry when an item ends, and while it follows the group turns the user's playback actions into group requests (GroupPlayback). Not following (the player was closed, or the user chose it) the session stays a member, the server stops waiting for it and its playback is its own until it follows again. A frame of another group, a command older than the last one or the same one again, one emitted more than a minute ago, and a command for an entry this session has not loaded do nothing; the server sends again what a member needs after its Ready. The server takes a session out of its group when its socket closes (observed): a socket that comes back joins again, and the session's end (sign-out, an expired token) drops the group without a request
class SyncPlayController final : public QObject, public GroupPlayback {
    Q_OBJECT
    Q_PROPERTY(Kinefin::SyncPlayController::Status status READ status NOTIFY statusChanged)
    // The group's name and members as the server last told them: plain text only, a name that cannot be shown is empty
    Q_PROPERTY(QString groupName READ groupName NOTIFY groupChanged)
    Q_PROPERTY(QStringList participants READ participants NOTIFY groupChanged)
    // The group waits for a member (loading, seeking, buffering)
    Q_PROPERTY(bool waiting READ waiting NOTIFY groupChanged)
    // The groups to join, for the dialog
    Q_PROPERTY(Kinefin::SyncPlayGroupsModel *groups READ groups CONSTANT)
    // A group can be joined now: signed in, the socket up and the user's policy not `None` (an unknown policy counts as allowed: the server decides); `canCreate` also needs a policy other than `JoinGroups`
    Q_PROPERTY(bool available READ available NOTIFY availabilityChanged)
    Q_PROPERTY(bool canCreate READ canCreate NOTIFY availabilityChanged)
    // Joined and keeping in step; false after stopFollowing() until resumeFollowing()
    Q_PROPERTY(bool following READ following NOTIFY followingChanged)
    // Joined while the socket is down: nothing is sent until it is back and the group joined again
    Q_PROPERTY(bool offline READ offline NOTIFY offlineChanged)
    // The group's queue, and its shuffle and repeat (0 off, 1 all, 2 one)
    Q_PROPERTY(Kinefin::SyncPlayQueueModel *queue READ queue CONSTANT)
    Q_PROPERTY(bool groupShuffle READ shuffle NOTIFY queueChanged)
    Q_PROPERTY(int groupRepeatMode READ repeatMode NOTIFY queueChanged)

  public:
    enum class Status { Idle, Joining, Joined };
    Q_ENUM(Status)
    // RequestFailed: a pause, unpause, seek, queue change or new queue the server did not take (the group goes on)
    enum class Failure {
        CreateDenied,
        JoinDenied,
        GroupGone,
        LibraryAccessDenied,
        NoAnswer,
        RequestFailed
    };
    Q_ENUM(Failure)

    SyncPlayController(JellyfinClient &client, JellyfinSocketClient &socket,
                       PlayerController &player, QObject *parent = nullptr);
    ~SyncPlayController() override;

    [[nodiscard]] Status status() const { return m_status; }
    [[nodiscard]] QString groupId() const { return m_groupId; }
    [[nodiscard]] QString groupName() const { return m_group.name; }
    [[nodiscard]] QStringList participants() const { return m_group.participants; }
    [[nodiscard]] bool waiting() const { return m_groupState == SyncPlay::GroupState::Waiting; }
    [[nodiscard]] SyncPlayGroupsModel *groups() { return &m_groups; }
    [[nodiscard]] bool available() const { return m_available; }
    [[nodiscard]] bool canCreate() const { return m_canCreate; }
    [[nodiscard]] bool following() const { return m_status == Status::Joined && m_following; }
    [[nodiscard]] bool offline() const { return m_offline; }
    [[nodiscard]] SyncPlayQueueModel *queue() { return &m_queueModel; }

    // A new group named `name`, or the group `groupId` (either GUID spelling): false, with nothing sent, unless idle with an authenticated socket (the group's answers arrive only on it) and a name or id the request takes. Joined follows the server's GroupJoined; a refusal, a failed request or no answer within the join timeout ends in failed() and Idle
    Q_INVOKABLE bool createGroup(const QString &name);
    Q_INVOKABLE bool joinGroup(const QString &groupId);
    // Leaves at once, locally; the request goes out when the session can still send it. Local playback goes on, no longer routed
    Q_INVOKABLE void leaveGroup();
    Q_INVOKABLE void stopFollowing() override;
    // Follows again and catches up: the group's entry opened or moved to where the group is, playing or paused as the group
    Q_INVOKABLE void resumeFollowing();
    // The group's queue: items after the playing entry (`next`) or at the end, an entry removed, moved (to its index after the move) or made the playing one. False, with nothing sent, unless joined and online with an entry or ids the request takes
    Q_INVOKABLE bool queueItems(const QStringList &itemIds, bool next);
    Q_INVOKABLE bool removeEntry(const QString &playlistItemId);
    Q_INVOKABLE bool moveEntry(const QString &playlistItemId, int newIndex);
    Q_INVOKABLE bool playEntry(const QString &playlistItemId);

    [[nodiscard]] bool routesPlayback() const override
    {
        return m_status == Status::Joined && m_following && !m_offline && !m_rejoining;
    }
    void requestPause() override;
    void requestUnpause() override;
    void requestSeek(qint64 ticks) override;
    // At most SyncPlay::MaxNewQueueItems of `itemIds`, around `index`
    bool requestQueue(const QStringList &itemIds, int index, qint64 startTicks) override;
    [[nodiscard]] bool hasNext() const override;
    [[nodiscard]] bool hasPrevious() const override;
    bool requestNext() override;
    bool requestPrevious() override;
    bool itemEnded(const QString &itemId) override;
    [[nodiscard]] bool shuffle() const override;
    [[nodiscard]] int repeatMode() const override;
    void requestShuffle(bool shuffle) override;
    void requestRepeatMode(int mode) override;

    // Tests only, in ms (a non-positive value keeps the production one): the join timeout, the interval of the first clock samples and the one after them, the drift check's
    void setTestTimings(int joinTimeoutMs, int firstSamplesMs, int laterSamplesMs,
                        int driftCheckMs = 0);

  Q_SIGNALS:
    void statusChanged();
    void groupChanged();
    // A member came or went; `name` is empty when it cannot be shown
    void memberJoined(const QString &name);
    void memberLeft(const QString &name);
    void failed(Kinefin::SyncPlayController::Failure failure);
    void availabilityChanged();
    void followingChanged();
    void offlineChanged();
    void queueChanged();
    // The group's item started loading in the player: the shell shows it
    void itemOpened();
    // The playback drifted away too often to be brought back: it keeps playing out of step until the group's next pause, unpause or seek
    void fellOutOfStep();

  private:
    // A play-state or queue request in flight, and the latest one waiting behind it
    struct RequestSlot {
        bool inFlight = false;
        std::optional<std::pair<SyncPlay::Route, QJsonObject>> pending;
    };

    void startJoining(SyncPlay::Route route, const QJsonObject &body, const QString &groupId);
    void fail(Failure failure, bool leave);
    // Back to Idle; `leave` sends Leave first, for a group the server may still count this session in
    void enterIdle(bool leave);
    void onSocketState();
    void onGroupUpdate(const SyncPlay::GroupUpdate &update);
    void onCommand(const SyncPlay::Command &command);
    void onQueue(const SyncPlay::PlayQueue &queue);
    void carryOut(const SyncPlay::Command &command);
    void runScheduled();
    void cancelScheduled();
    // Where the group's playback is by now, from its last pause or unpause
    [[nodiscard]] qint64 expectedPositionTicks() const;
    // The playback takes the group's state: playing from where the group is, or paused where it paused; Ready when the group waits
    void applyGroupState();
    void sampleClock();
    void onUtcTime(qint64 receptionMs, qint64 transmissionMs);
    void scheduleSample();
    void onPlayerChanged();
    void sendReadyWhenSettled();
    void updateBuffering();
    void checkDrift();
    void restoreRate();
    void sendIn(RequestSlot &slot, SyncPlay::Route route, const QJsonObject &body);
    // A queue change of the group: sent unless the body does not fit
    bool sendQueueChange(SyncPlay::Route route, const QJsonObject &body);
    void onRequestFinished(SyncPlay::Route route, bool accepted, int httpStatus);
    void updateAvailability();
    [[nodiscard]] qint64 serverNowMs() const;

    JellyfinClient &m_client;
    JellyfinSocketClient &m_socket;
    PlayerController &m_player;
    ServerClock m_clock;
    Status m_status = Status::Idle;
    bool m_following = true;
    bool m_offline = false;
    // A join sent again after the socket came back; its GroupJoined is followed by a Stop that must not stop anything
    bool m_rejoining = false;
    // Until when (monotonic ms) the Stop that follows a join again is ignored
    qint64 m_ignoreJoinStopUntilMs = 0;
    // A (re)join: the first queue sent after it wants a Ready even for an entry already open here
    bool m_readyAfterJoin = false;
    // The group a join asked for, empty for a create (any GroupJoined answers it)
    QString m_joiningGroupId;
    QString m_groupId;
    SyncPlay::GroupInfo m_group;
    SyncPlay::GroupState m_groupState = SyncPlay::GroupState::Idle;
    SyncPlay::PlayQueue m_queue;
    // The group's playing entry and its item, empty when nothing is current; the newest queue update seen
    QString m_entry;
    QString m_entryItem;
    qint64 m_lastQueueUpdateMs = 0;
    // The entry was opened or seeked and the server waits for this session's Ready
    bool m_readyWanted = false;
    // Following again: once the entry has settled it takes the group's state
    bool m_catchUp = false;
    std::optional<SyncPlay::Command> m_lastCommand;
    // The group's last pause or unpause of the playing entry: where the playback should be by now
    std::optional<SyncPlay::Command> m_lastPlayCommand;
    // The commands that came before the first clock sample, kept a short while, in order
    QList<SyncPlay::Command> m_heldCommands;
    QTimer m_heldExpiry;
    // A pause or unpause waiting for its moment
    std::optional<SyncPlay::Command> m_scheduled;
    QTimer m_commandTimer;
    QTimer m_joinTimer;
    QTimer m_sampleTimer;
    QTimer m_jumpCheck;
    int m_samplesTaken = 0;
    qint64 m_sampleSentMs = -1;
    // A cache stall lasting long enough is the group's Buffering, its end a Ready
    QTimer m_bufferDelay;
    bool m_bufferingSent = false;
    // The drift correction: a speed off 1.0 in force, the skips of the last 30 s, a pause after a skip, how long a skip took, whether it gave up
    QTimer m_driftCheck;
    bool m_rateNudged = false;
    QList<qint64> m_skips;
    qint64 m_driftQuietUntilMs = 0;
    qint64 m_skipStartedMs = -1;
    qint64 m_skipLatencyMs = 0;
    bool m_outOfStep = false;
    RequestSlot m_playState;
    RequestSlot m_queueRequest;
    // NextItem and PreviousItem, apart from a new queue so that neither replaces the other
    RequestSlot m_stepRequest;
    int m_firstSamplesMs;
    int m_laterSamplesMs;
    SyncPlayGroupsModel m_groups;
    SyncPlayQueueModel m_queueModel;
    bool m_available = false;
    bool m_canCreate = false;
};

} // namespace Kinefin
