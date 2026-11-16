// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/SyncPlayProtocol.h"
#include "player/GroupPlayback.h"
#include "syncplay/ServerClock.h"
#include "syncplay/SyncPlayGroupsModel.h"

#include <QObject>
#include <QStringList>
#include <QTimer>

#include <optional>
#include <utility>

namespace Kinefin {
class JellyfinClient;
class JellyfinSocketClient;
class PlayerController;

// Watching together through the server's SyncPlay groups (the wire format is SyncPlayProtocol.h). It creates, joins and leaves a group, keeps the server's clock, opens the group's playing item paused, tells the server Ready once that item (or a seek in it) has settled, carries out the group's pause, unpause and seek at server time and its stop at once, and while joined turns the user's playback actions into group requests (GroupPlayback). A frame of another group, a command older than the last one or the same one again, one emitted more than a minute ago, and a command for an entry this session has not loaded do nothing; the server sends again what a member needs after its Ready. The session's end (sign-out, an expired token) drops the group without a request: the server takes the session out of its group when the socket closes (observed)
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

  public:
    enum class Status { Idle, Joining, Joined };
    Q_ENUM(Status)
    // RequestFailed: a pause, unpause, seek or new queue the server did not take (the group goes on)
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

    // A new group named `name`, or the group `groupId` (either GUID spelling): false, with nothing sent, unless idle with an authenticated socket (the group's answers arrive only on it) and a name or id the request takes. Joined follows the server's GroupJoined; a refusal, a failed request or no answer within the join timeout ends in failed() and Idle
    Q_INVOKABLE bool createGroup(const QString &name);
    Q_INVOKABLE bool joinGroup(const QString &groupId);
    // Leaves at once, locally; the request goes out when the session can still send it. Local playback goes on, no longer routed
    Q_INVOKABLE void leaveGroup();

    [[nodiscard]] bool routesPlayback() const override { return m_status == Status::Joined; }
    void requestPause() override;
    void requestUnpause() override;
    void requestSeek(qint64 ticks) override;
    // At most SyncPlay::MaxNewQueueItems of `itemIds`, around `index`
    bool requestQueue(const QStringList &itemIds, int index, qint64 startTicks) override;

    // Tests only, in ms (a non-positive value keeps the production one): the join timeout, the interval of the first clock samples and the one after them
    void setTestTimings(int joinTimeoutMs, int firstSamplesMs, int laterSamplesMs);

  Q_SIGNALS:
    void statusChanged();
    void groupChanged();
    // A member came or went; `name` is empty when it cannot be shown
    void memberJoined(const QString &name);
    void memberLeft(const QString &name);
    void failed(Kinefin::SyncPlayController::Failure failure);
    void availabilityChanged();
    // The group's item started loading in the player: the shell shows it
    void itemOpened();

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
    void onGroupUpdate(const SyncPlay::GroupUpdate &update);
    void onCommand(const SyncPlay::Command &command);
    void onQueue(const SyncPlay::PlayQueue &queue);
    void carryOut(const SyncPlay::Command &command);
    void runScheduled();
    void sampleClock();
    void onUtcTime(qint64 receptionMs, qint64 transmissionMs);
    void scheduleSample();
    void sendReadyWhenSettled();
    void sendIn(RequestSlot &slot, SyncPlay::Route route, const QJsonObject &body);
    void onRequestFinished(SyncPlay::Route route, bool accepted, int httpStatus);
    void updateAvailability();

    JellyfinClient &m_client;
    JellyfinSocketClient &m_socket;
    PlayerController &m_player;
    ServerClock m_clock;
    Status m_status = Status::Idle;
    // The group a join asked for, empty for a create (any GroupJoined answers it)
    QString m_joiningGroupId;
    QString m_groupId;
    SyncPlay::GroupInfo m_group;
    SyncPlay::GroupState m_groupState = SyncPlay::GroupState::Idle;
    // The group's playing entry and its item, empty when nothing is current; the newest queue update seen
    QString m_entry;
    QString m_entryItem;
    qint64 m_lastQueueUpdateMs = 0;
    // The entry was opened or seeked and the server waits for this session's Ready
    bool m_readyWanted = false;
    std::optional<SyncPlay::Command> m_lastCommand;
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
    RequestSlot m_playState;
    RequestSlot m_queueRequest;
    int m_firstSamplesMs;
    int m_laterSamplesMs;
    SyncPlayGroupsModel m_groups;
    bool m_available = false;
    bool m_canCreate = false;
};

} // namespace Kinefin
