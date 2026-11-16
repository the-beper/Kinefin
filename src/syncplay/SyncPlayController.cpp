// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "syncplay/SyncPlayController.h"

#include "api/JellyfinClient.h"
#include "api/JellyfinSocketClient.h"
#include "player/PlayerController.h"

#include <QLoggingCategory>

#include <algorithm>
#include <cstdlib>

Q_LOGGING_CATEGORY(syncPlayLog, "kinefin.syncplay")

namespace Kinefin {
namespace {

constexpr int JoinTimeoutMs = 10'000;
// The clock: eight quick samples, then one a minute (as jellyfin-web does)
constexpr int FirstSamples = 8;
constexpr int FirstSamplesMs = 1000;
constexpr int LaterSamplesMs = 60'000;
// How long the commands that came before the first clock sample are kept, and how many
constexpr int HeldCommandMs = 2000;
constexpr int MaxHeldCommands = 4;
// A command emitted longer ago than this is stale
constexpr qint64 StaleCommandMs = 60'000;
// A command's moment more than this far ahead is brought forward to it (a hostile or confused `When`)
constexpr qint64 LongestWaitMs = 10'000;
// A position closer than this to where a command puts the playback is left alone (100 ms in ticks)
constexpr qint64 PositionToleranceTicks = 1'000'000;
constexpr qint64 TicksPerMs = 10'000;
// How long a cache stall lasts before it is the group's Buffering
constexpr int BufferDelayMs = 500;
// The drift correction, every second: under 100 ms nothing; up to a second a speed off 1.0 by the drift over 4 s, at most 10 %, until under 30 ms; from a second a skip, then a pause of 3 s; more than three skips in 30 s give up
constexpr int DriftCheckMs = 1000;
constexpr qint64 DriftIgnoredMs = 100;
constexpr qint64 DriftSettledMs = 30;
constexpr qint64 DriftSkipMs = 1000;
constexpr double DriftRateSpanMs = 4000.0;
constexpr double DriftMaxRateChange = 0.1;
constexpr qint64 SkipQuietMs = 3000;
constexpr qint64 SkipWindowMs = 30'000;
constexpr int MaxSkips = 3;

bool isPlayState(SyncPlay::Route route)
{
    return route == SyncPlay::Route::Pause || route == SyncPlay::Route::Unpause
           || route == SyncPlay::Route::Seek;
}

bool isQueueRoute(SyncPlay::Route route)
{
    return route == SyncPlay::Route::SetNewQueue || route == SyncPlay::Route::NextItem
           || route == SyncPlay::Route::PreviousItem || route == SyncPlay::Route::SetPlaylistItem
           || route == SyncPlay::Route::Queue || route == SyncPlay::Route::RemoveFromPlaylist
           || route == SyncPlay::Route::MovePlaylistItem || route == SyncPlay::Route::SetRepeatMode
           || route == SyncPlay::Route::SetShuffleMode;
}

// A GUID in the 32-digit lower-case form the frames are parsed into
QString canonicalId(QString id)
{
    return id.remove(QLatin1Char('-')).toLower();
}

} // namespace

SyncPlayController::SyncPlayController(JellyfinClient &client, JellyfinSocketClient &socket,
                                       PlayerController &player, QObject *parent)
    : QObject(parent), m_client(client), m_socket(socket), m_player(player),
      m_firstSamplesMs(FirstSamplesMs), m_laterSamplesMs(LaterSamplesMs), m_groups(&client),
      m_queueModel(&client)
{
    connect(&m_socket, &JellyfinSocketClient::stateChanged, this,
            &SyncPlayController::onSocketState);
    connect(&m_client, &JellyfinClient::loginSucceeded, this,
            &SyncPlayController::updateAvailability);
    connect(&m_client, &JellyfinClient::userProfileRefreshed, this,
            &SyncPlayController::updateAvailability);
    connect(&m_client, &JellyfinClient::credentialsCleared, this,
            &SyncPlayController::updateAvailability);
    m_joinTimer.setSingleShot(true);
    m_joinTimer.setInterval(JoinTimeoutMs);
    connect(&m_joinTimer, &QTimer::timeout, this, [this] { fail(Failure::NoAnswer, true); });
    m_heldExpiry.setSingleShot(true);
    m_heldExpiry.setInterval(HeldCommandMs);
    connect(&m_heldExpiry, &QTimer::timeout, this, [this] { m_heldCommands.clear(); });
    m_commandTimer.setSingleShot(true);
    m_commandTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_commandTimer, &QTimer::timeout, this, &SyncPlayController::runScheduled);
    m_sampleTimer.setSingleShot(true);
    connect(&m_sampleTimer, &QTimer::timeout, this, &SyncPlayController::sampleClock);
    // A suspend or a clock step leaves the samples wrong: take them again
    m_jumpCheck.setInterval(1000);
    connect(&m_jumpCheck, &QTimer::timeout, this, [this] {
        if (!m_clock.dropAfterJump())
            return;
        qCInfo(syncPlayLog) << "the clock jumped, sampling the server's time again";
        m_samplesTaken = 0;
        sampleClock();
    });
    m_bufferDelay.setSingleShot(true);
    m_bufferDelay.setInterval(BufferDelayMs);
    connect(&m_bufferDelay, &QTimer::timeout, this, [this] {
        if (!routesPlayback() || !m_player.buffering() || !m_player.groupItemLoaded(m_entryItem))
            return;
        m_bufferingSent = true;
        m_client.postSyncPlay(
            SyncPlay::Route::Buffering,
            SyncPlay::readinessBody(serverNowMs(), m_player.positionTicks(), true, m_entry));
    });
    m_driftCheck.setInterval(DriftCheckMs);
    connect(&m_driftCheck, &QTimer::timeout, this, &SyncPlayController::checkDrift);

    connect(&m_socket, &JellyfinSocketClient::syncPlayGroupUpdate, this,
            &SyncPlayController::onGroupUpdate);
    connect(&m_socket, &JellyfinSocketClient::syncPlayCommand, this,
            &SyncPlayController::onCommand);
    connect(&m_client, &JellyfinClient::utcTimeReady, this, &SyncPlayController::onUtcTime);
    connect(&m_client, &JellyfinClient::utcTimeFailed, this, [this] {
        m_sampleSentMs = -1;
        scheduleSample();
    });
    connect(&m_client, &JellyfinClient::syncPlayRequestFinished, this,
            &SyncPlayController::onRequestFinished);
    // The requests the client held are gone without a word, and the server drops the session from its group with the socket
    connect(&m_client, &JellyfinClient::sessionExpired, this, [this] { enterIdle(false); });
    connect(&m_client, &JellyfinClient::credentialsCleared, this, [this] { enterIdle(false); });
    connect(&m_player, &PlayerController::changed, this, &SyncPlayController::onPlayerChanged);
    // `following` reads the status too
    connect(this, &SyncPlayController::statusChanged, this, &SyncPlayController::followingChanged);
    updateAvailability();
}

SyncPlayController::~SyncPlayController()
{
    // The player outlives this object in main.cpp: it must not route to it afterwards
    m_player.setGroupPlayback(nullptr);
}

void SyncPlayController::setTestTimings(int joinTimeoutMs, int firstSamplesMs, int laterSamplesMs,
                                        int driftCheckMs)
{
    if (joinTimeoutMs > 0)
        m_joinTimer.setInterval(joinTimeoutMs);
    if (firstSamplesMs > 0)
        m_firstSamplesMs = firstSamplesMs;
    if (laterSamplesMs > 0)
        m_laterSamplesMs = laterSamplesMs;
    if (driftCheckMs > 0)
        m_driftCheck.setInterval(driftCheckMs);
}

qint64 SyncPlayController::serverNowMs() const
{
    return m_clock.serverNowMs().value_or(0);
}

bool SyncPlayController::createGroup(const QString &name)
{
    const QJsonObject body = SyncPlay::newGroupBody(name);
    if (!SyncPlay::isValidRequest(SyncPlay::Route::New, body))
        return false;
    startJoining(SyncPlay::Route::New, body, {});
    return m_status == Status::Joining;
}

bool SyncPlayController::joinGroup(const QString &groupId)
{
    const QJsonObject body = SyncPlay::joinBody(groupId);
    if (!SyncPlay::isValidRequest(SyncPlay::Route::Join, body))
        return false;
    startJoining(SyncPlay::Route::Join, body, canonicalId(groupId));
    return m_status == Status::Joining;
}

void SyncPlayController::startJoining(SyncPlay::Route route, const QJsonObject &body,
                                      const QString &groupId)
{
    if (m_status != Status::Idle || !m_client.isAuthenticated()
        || m_socket.state() != JellyfinSocketClient::State::Authenticated
        || !m_client.postSyncPlay(route, body))
        return;
    m_status = Status::Joining;
    m_joiningGroupId = groupId;
    m_following = true;
    m_joinTimer.start();
    m_jumpCheck.start();
    m_samplesTaken = 0;
    sampleClock();
    Q_EMIT statusChanged();
}

void SyncPlayController::leaveGroup()
{
    if (m_status != Status::Idle)
        enterIdle(true);
}

void SyncPlayController::fail(Failure failure, bool leave)
{
    enterIdle(leave);
    Q_EMIT failed(failure);
}

void SyncPlayController::enterIdle(bool leave)
{
    if (m_status == Status::Idle)
        return;
    if (leave && m_client.isAuthenticated() && !m_offline)
        m_client.postSyncPlay(SyncPlay::Route::Leave, {});
    const bool wasOffline = m_offline;
    // What plays goes on as the user's own, at its own speed
    restoreRate();
    m_status = Status::Idle;
    m_player.releaseGroupItem();
    m_following = true;
    m_offline = false;
    m_rejoining = false;
    m_ignoreJoinStopUntilMs = 0;
    m_readyAfterJoin = false;
    m_joiningGroupId.clear();
    m_groupId.clear();
    m_group = {};
    m_groupState = SyncPlay::GroupState::Idle;
    m_queue = {};
    m_entry.clear();
    m_entryItem.clear();
    m_lastQueueUpdateMs = 0;
    m_readyWanted = false;
    m_catchUp = false;
    m_lastCommand.reset();
    m_lastPlayCommand.reset();
    m_heldCommands.clear();
    m_heldExpiry.stop();
    cancelScheduled();
    m_joinTimer.stop();
    m_sampleTimer.stop();
    m_jumpCheck.stop();
    m_sampleSentMs = -1;
    m_clock.reset();
    m_bufferDelay.stop();
    m_bufferingSent = false;
    m_driftCheck.stop();
    m_skips.clear();
    m_driftQuietUntilMs = 0;
    m_skipStartedMs = -1;
    m_outOfStep = false;
    m_playState = {};
    m_queueRequest = {};
    m_stepRequest = {};
    m_queueModel.clear();
    Q_EMIT statusChanged();
    Q_EMIT groupChanged();
    Q_EMIT queueChanged();
    if (wasOffline)
        Q_EMIT offlineChanged();
    m_player.notifyGroupChanged();
}

void SyncPlayController::onSocketState()
{
    updateAvailability();
    if (m_status != Status::Joined)
        return;
    if (m_socket.state() != JellyfinSocketClient::State::Authenticated) {
        if (m_offline)
            return;
        // The server takes the session out of its group with the socket: keep playing, send nothing. A join again in flight waits for the next time the socket is back
        m_offline = true;
        m_rejoining = false;
        m_joinTimer.stop();
        cancelScheduled();
        m_bufferDelay.stop();
        m_bufferingSent = false;
        restoreRate();
        Q_EMIT offlineChanged();
        m_player.notifyGroupChanged();
        return;
    }
    if (!m_offline || !m_client.isAuthenticated())
        return;
    // Back: the server never joins a session again by itself
    if (!m_client.postSyncPlay(SyncPlay::Route::Join, SyncPlay::joinBody(m_groupId))) {
        fail(Failure::NoAnswer, false);
        return;
    }
    m_offline = false;
    m_rejoining = true;
    m_joinTimer.start();
    m_samplesTaken = 0;
    sampleClock();
    Q_EMIT offlineChanged();
    m_player.notifyGroupChanged();
}

void SyncPlayController::onGroupUpdate(const SyncPlay::GroupUpdate &update)
{
    using Type = SyncPlay::GroupUpdate::Type;
    if (m_status == Status::Idle)
        return;
    const bool ours = m_status == Status::Joined && update.groupId == m_groupId;
    switch (update.type) {
    case Type::GroupJoined:
        if (ours) {
            m_group = update.group;
            if (std::exchange(m_rejoining, false)) {
                m_joinTimer.stop();
                m_ignoreJoinStopUntilMs = m_clock.monotonicNowMs() + 5000;
                m_readyAfterJoin = true;
                // A member that joins again is a new one to the server: it waits for it unless told again
                if (!m_following)
                    m_client.postSyncPlay(SyncPlay::Route::SetIgnoreWait,
                                          SyncPlay::ignoreWaitBody(true));
                m_player.notifyGroupChanged();
            }
            Q_EMIT groupChanged();
            return;
        }
        if (m_status != Status::Joining
            || (!m_joiningGroupId.isEmpty() && update.groupId != m_joiningGroupId))
            return;
        m_status = Status::Joined;
        m_joinTimer.stop();
        m_groupId = update.groupId;
        m_group = update.group;
        m_groupState = update.group.state;
        m_readyAfterJoin = true;
        m_driftCheck.start();
        Q_EMIT statusChanged();
        Q_EMIT groupChanged();
        m_player.notifyGroupChanged();
        return;
    case Type::UserJoined:
    case Type::UserLeft:
        if (!ours)
            return;
        if (update.type == Type::UserJoined) {
            if (!update.userName.isEmpty() && !m_group.participants.contains(update.userName)
                && m_group.participants.size() < SyncPlay::MaxParticipants)
                m_group.participants.append(update.userName);
            Q_EMIT memberJoined(update.userName);
        } else {
            m_group.participants.removeOne(update.userName);
            Q_EMIT memberLeft(update.userName);
        }
        Q_EMIT groupChanged();
        return;
    case Type::GroupLeft:
        if (ours)
            enterIdle(false);
        return;
    case Type::StateUpdate:
        if (ours && m_groupState != update.state) {
            m_groupState = update.state;
            Q_EMIT groupChanged();
            // A group that waits again wants this session's Ready after a catch-up
            sendReadyWhenSettled();
        }
        return;
    case Type::PlayQueue:
        if (ours)
            onQueue(update.queue);
        return;
    case Type::NotInGroup:
        // While joining it may answer an older request; once joined (and not joining again) the server has forgotten this session
        if (m_status == Status::Joined && !m_rejoining)
            fail(Failure::GroupGone, false);
        return;
    case Type::GroupDoesNotExist:
        fail(Failure::GroupGone, false);
        return;
    case Type::CreateGroupDenied:
        if (m_status == Status::Joining)
            fail(Failure::CreateDenied, false);
        return;
    case Type::JoinGroupDenied:
        if (m_status == Status::Joining || m_rejoining)
            fail(Failure::JoinDenied, false);
        return;
    case Type::LibraryAccessDenied:
        fail(Failure::LibraryAccessDenied, m_status == Status::Joined);
        return;
    }
}

void SyncPlayController::onQueue(const SyncPlay::PlayQueue &queue)
{
    if (queue.lastUpdateMs < m_lastQueueUpdateMs)
        return;
    m_lastQueueUpdateMs = queue.lastUpdateMs;
    m_queue = queue;
    m_queueModel.setQueue(queue.entries, queue.playingIndex);
    Q_EMIT queueChanged();
    m_player.notifyGroupChanged();
    const bool afterJoin = std::exchange(m_readyAfterJoin, false);
    if (queue.playingIndex < 0) {
        const bool playing = !m_entryItem.isEmpty() && m_player.itemId() == m_entryItem;
        cancelScheduled();
        m_entry.clear();
        m_entryItem.clear();
        m_readyWanted = false;
        m_catchUp = false;
        m_lastPlayCommand.reset();
        if (playing && m_following)
            m_player.applyStop();
        return;
    }
    const SyncPlay::QueueEntry &entry = queue.entries.at(queue.playingIndex);
    if (entry.playlistItemId == m_entry && m_player.itemId() == entry.itemId) {
        // The item ended: only the answer to its NextItem moves on, the same entry again under a repeat included; any other edit leaves the player waiting
        if (m_player.awaitingGroup() && queue.reason != QLatin1String("NextItem"))
            return;
        // An edit that keeps the playing entry, already open here, changes nothing for the player: a reorder, a shuffle, a repeat mode
        if (!m_player.awaitingGroup()) {
            // After a (re)join the group waits for this session's Ready
            if (afterJoin && m_following) {
                m_readyWanted = true;
                sendReadyWhenSettled();
            }
            return;
        }
    }
    cancelScheduled();
    m_entry = entry.playlistItemId;
    m_entryItem = entry.itemId;
    m_lastPlayCommand.reset();
    m_catchUp = false;
    // Not following: the entry opens when the session follows again
    if (!m_following) {
        m_readyWanted = false;
        return;
    }
    m_readyWanted = m_player.openGroupItem(entry.itemId, queue.startPositionTicks);
    if (m_readyWanted)
        Q_EMIT itemOpened();
}

void SyncPlayController::onCommand(const SyncPlay::Command &command)
{
    if (m_status != Status::Joined || command.groupId != m_groupId)
        return;
    // Older than the last one, or the same one again; two commands may share a millisecond
    if (m_lastCommand
        && (command.emittedAtMs < m_lastCommand->emittedAtMs
            || (command.emittedAtMs == m_lastCommand->emittedAtMs
                && command.kind == m_lastCommand->kind && command.whenMs == m_lastCommand->whenMs
                && command.positionTicks == m_lastCommand->positionTicks
                && command.playlistItemId == m_lastCommand->playlistItemId))) {
        qCDebug(syncPlayLog) << "an older or repeated command is dropped";
        return;
    }
    m_lastCommand = command;
    if (command.kind == SyncPlay::Command::Kind::Stop) {
        // The Stop after a join again must not stop what goes on
        const bool afterRejoin =
            m_clock.monotonicNowMs() < std::exchange(m_ignoreJoinStopUntilMs, 0);
        if (afterRejoin && command.playlistItemId.isEmpty())
            return;
        // Not a moment to wait for: also the one that follows every join, which stops what played alone
        cancelScheduled();
        m_readyWanted = false;
        m_lastPlayCommand.reset();
        if (m_following && m_player.active())
            m_player.applyStop();
        return;
    }
    // Kept also while not following: following again starts from it
    if (command.kind == SyncPlay::Command::Kind::Pause
        || command.kind == SyncPlay::Command::Kind::Unpause)
        m_lastPlayCommand = command;
    else
        m_lastPlayCommand.reset();
    if (!m_following)
        return;
    if (!m_clock.hasSample()) {
        if (m_heldCommands.size() == MaxHeldCommands)
            m_heldCommands.removeFirst();
        m_heldCommands.append(command);
        m_heldExpiry.start();
        return;
    }
    carryOut(command);
}

void SyncPlayController::carryOut(const SyncPlay::Command &command)
{
    if (serverNowMs() - command.emittedAtMs > StaleCommandMs) {
        qCDebug(syncPlayLog) << "a stale command" << int(command.kind) << "is dropped";
        return;
    }
    if (command.playlistItemId.isEmpty() || command.playlistItemId != m_entry
        || !m_player.groupItemLoaded(m_entryItem)) {
        qCDebug(syncPlayLog) << "a command" << int(command.kind)
                             << "for an entry not loaded here is dropped";
        return;
    }
    cancelScheduled();
    // The group's own pause, unpause or seek: the correction may try again
    m_outOfStep = false;
    m_skips.clear();
    if (command.kind == SyncPlay::Command::Kind::Seek) {
        // The group waits for everyone's Ready at the new position
        m_player.applyPause();
        m_player.applySeek(command.positionTicks);
        m_readyWanted = true;
        // A seek that did not start (a stream that cannot seek) changes nothing to wait for
        sendReadyWhenSettled();
        return;
    }
    const qint64 wait =
        m_clock.monotonicForServerMs(command.whenMs).value_or(0) - m_clock.monotonicNowMs();
    qCDebug(syncPlayLog) << "command" << int(command.kind) << "in" << wait << "ms";
    m_scheduled = command;
    if (wait <= 0)
        runScheduled();
    else
        m_commandTimer.start(int(std::min(wait, LongestWaitMs)));
}

void SyncPlayController::runScheduled()
{
    if (!m_scheduled)
        return;
    const SyncPlay::Command command = *std::exchange(m_scheduled, std::nullopt);
    if (command.playlistItemId != m_entry || !m_player.groupItemLoaded(m_entryItem))
        return;
    const qint64 position = m_player.positionTicks();
    if (command.kind == SyncPlay::Command::Kind::Pause) {
        restoreRate();
        m_player.applyPause();
        if (command.positionTicks >= 0
            && std::llabs(position - command.positionTicks) > PositionToleranceTicks)
            m_player.applySeek(command.positionTicks);
        return;
    }
    // An unpause whose moment has passed plays from where the group is by now
    if (command.positionTicks >= 0) {
        const qint64 late = std::max<qint64>(0, serverNowMs() - command.whenMs);
        const qint64 target = command.positionTicks + late * TicksPerMs;
        if (std::llabs(position - target) > PositionToleranceTicks)
            m_player.applySeek(target);
    }
    m_player.applyUnpause();
}

void SyncPlayController::cancelScheduled()
{
    m_scheduled.reset();
    m_commandTimer.stop();
}

qint64 SyncPlayController::expectedPositionTicks() const
{
    if (!m_lastPlayCommand || m_lastPlayCommand->playlistItemId != m_entry)
        return m_queue.startPositionTicks;
    const qint64 position = std::max<qint64>(0, m_lastPlayCommand->positionTicks);
    if (m_lastPlayCommand->kind != SyncPlay::Command::Kind::Unpause || !m_clock.hasSample())
        return position;
    return position + std::max<qint64>(0, serverNowMs() - m_lastPlayCommand->whenMs) * TicksPerMs;
}

void SyncPlayController::applyGroupState()
{
    if (!m_player.groupItemLoaded(m_entryItem))
        return;
    if (m_lastPlayCommand && m_lastPlayCommand->playlistItemId == m_entry) {
        m_scheduled = *m_lastPlayCommand;
        runScheduled();
        return;
    }
    if (m_groupState == SyncPlay::GroupState::Waiting) {
        m_readyWanted = true;
        sendReadyWhenSettled();
    }
}

void SyncPlayController::stopFollowing()
{
    if (m_status != Status::Joined || !m_following)
        return;
    m_following = false;
    if (m_client.isAuthenticated() && !m_offline)
        m_client.postSyncPlay(SyncPlay::Route::SetIgnoreWait, SyncPlay::ignoreWaitBody(true));
    cancelScheduled();
    m_heldCommands.clear();
    m_readyWanted = false;
    m_catchUp = false;
    m_bufferDelay.stop();
    m_bufferingSent = false;
    restoreRate();
    // The playback is the user's own: its end, its speed and its repeat are no longer the group's
    m_player.releaseGroupItem();
    Q_EMIT followingChanged();
    m_player.notifyGroupChanged();
}

void SyncPlayController::resumeFollowing()
{
    if (m_status != Status::Joined || m_following || m_offline)
        return;
    m_following = true;
    m_client.postSyncPlay(SyncPlay::Route::SetIgnoreWait, SyncPlay::ignoreWaitBody(false));
    Q_EMIT followingChanged();
    m_player.notifyGroupChanged();
    if (m_entry.isEmpty())
        return;
    if (m_player.groupItemLoaded(m_entryItem)) {
        applyGroupState();
        return;
    }
    // The entry opens where the group is by now, and takes its state once it settled
    m_catchUp = m_player.openGroupItem(m_entryItem, expectedPositionTicks());
    if (m_catchUp)
        Q_EMIT itemOpened();
}

void SyncPlayController::sampleClock()
{
    if (m_status == Status::Idle)
        return;
    m_sampleSentMs = m_clock.monotonicNowMs();
    if (!m_client.fetchUtcTime()) {
        m_sampleSentMs = -1;
        scheduleSample();
    }
}

void SyncPlayController::onUtcTime(qint64 receptionMs, qint64 transmissionMs)
{
    if (m_status == Status::Idle || m_sampleSentMs < 0)
        return;
    const qint64 sent = std::exchange(m_sampleSentMs, -1);
    if (m_clock.addSample(sent, receptionMs, transmissionMs, m_clock.monotonicNowMs()))
        ++m_samplesTaken;
    // The server times its commands by the slowest member's round trip; a session that does not follow is not waited for
    if (routesPlayback())
        if (const auto roundTrip = m_clock.roundTripMs())
            m_client.postSyncPlay(SyncPlay::Route::Ping, SyncPlay::pingBody(std::min<qint64>(
                                                             *roundTrip, SyncPlay::MaxPingMs)));
    // The Ready an opened item waited for goes first, then what the group said meanwhile
    sendReadyWhenSettled();
    if (m_clock.hasSample()) {
        m_heldExpiry.stop();
        for (const SyncPlay::Command &held : std::exchange(m_heldCommands, {}))
            carryOut(held);
    }
    scheduleSample();
}

void SyncPlayController::scheduleSample()
{
    if (m_status == Status::Idle)
        return;
    m_sampleTimer.start(m_samplesTaken < FirstSamples ? m_firstSamplesMs : m_laterSamplesMs);
}

void SyncPlayController::onPlayerChanged()
{
    // How long a skip of the drift correction took, for the next one
    if (m_skipStartedMs >= 0 && !m_player.seeking() && m_player.groupItemSettled(m_entryItem)) {
        m_skipLatencyMs = std::clamp<qint64>(m_clock.monotonicNowMs() - m_skipStartedMs, 0, 2000);
        m_skipStartedMs = -1;
    }
    sendReadyWhenSettled();
    updateBuffering();
}

void SyncPlayController::sendReadyWhenSettled()
{
    if (!routesPlayback() || !m_clock.hasSample() || !m_player.groupItemSettled(m_entryItem))
        return;
    if (std::exchange(m_catchUp, false)) {
        applyGroupState();
        return;
    }
    if (!m_readyWanted)
        return;
    m_readyWanted = false;
    m_client.postSyncPlay(SyncPlay::Route::Ready,
                          SyncPlay::readinessBody(serverNowMs(), m_player.positionTicks(),
                                                  !m_player.paused(), m_entry));
}

void SyncPlayController::updateBuffering()
{
    if (!routesPlayback() || m_entry.isEmpty() || !m_player.groupItemLoaded(m_entryItem)) {
        m_bufferDelay.stop();
        return;
    }
    if (m_player.buffering()) {
        if (!m_bufferingSent && !m_bufferDelay.isActive())
            m_bufferDelay.start();
        return;
    }
    m_bufferDelay.stop();
    if (!std::exchange(m_bufferingSent, false))
        return;
    m_client.postSyncPlay(SyncPlay::Route::Ready,
                          SyncPlay::readinessBody(serverNowMs(), m_player.positionTicks(),
                                                  !m_player.paused(), m_entry));
}

void SyncPlayController::checkDrift()
{
    const bool inStep = routesPlayback() && m_clock.hasSample() && m_lastPlayCommand
                        && m_lastPlayCommand->kind == SyncPlay::Command::Kind::Unpause
                        && m_lastPlayCommand->playlistItemId == m_entry
                        && m_groupState == SyncPlay::GroupState::Playing
                        && m_player.groupItemSettled(m_entryItem) && !m_player.paused()
                        && !m_player.buffering() && !m_scheduled;
    if (!inStep) {
        restoreRate();
        return;
    }
    const qint64 now = m_clock.monotonicNowMs();
    if (m_outOfStep || now < m_driftQuietUntilMs)
        return;
    const qint64 driftMs = (m_player.positionTicks() - expectedPositionTicks()) / TicksPerMs;
    const qint64 size = std::llabs(driftMs);
    if (size >= DriftSkipMs) {
        restoreRate();
        m_skips.removeIf([now](qint64 at) { return now - at > SkipWindowMs; });
        if (m_skips.size() >= MaxSkips) {
            qCInfo(syncPlayLog) << "the playback keeps drifting, the correction stops";
            m_outOfStep = true;
            Q_EMIT fellOutOfStep();
            return;
        }
        m_skips.append(now);
        m_skipStartedMs = now;
        m_driftQuietUntilMs = now + SkipQuietMs;
        m_player.applySeek(expectedPositionTicks() + m_skipLatencyMs * TicksPerMs);
        return;
    }
    if (size >= DriftIgnoredMs) {
        // Ahead slows down, behind speeds up
        const double change =
            std::clamp(double(driftMs) / DriftRateSpanMs, -DriftMaxRateChange, DriftMaxRateChange);
        m_player.applyRate(1.0 - change);
        m_rateNudged = true;
        return;
    }
    if (size < DriftSettledMs)
        restoreRate();
}

void SyncPlayController::restoreRate()
{
    if (std::exchange(m_rateNudged, false))
        m_player.applyRate(1.0);
}

void SyncPlayController::requestPause()
{
    if (routesPlayback())
        sendIn(m_playState, SyncPlay::Route::Pause, {});
}

void SyncPlayController::requestUnpause()
{
    if (routesPlayback())
        sendIn(m_playState, SyncPlay::Route::Unpause, {});
}

void SyncPlayController::requestSeek(qint64 ticks)
{
    if (routesPlayback())
        sendIn(m_playState, SyncPlay::Route::Seek,
               SyncPlay::seekBody(std::clamp<qint64>(ticks, 0, SyncPlay::MaxPositionTicks)));
}

bool SyncPlayController::requestQueue(const QStringList &itemIds, int index, qint64 startTicks)
{
    if (!routesPlayback() || index < 0 || index >= itemIds.size())
        return false;
    const int size = int(itemIds.size());
    const int start = std::clamp(index - SyncPlay::MaxNewQueueItems / 2, 0,
                                 std::max(0, size - SyncPlay::MaxNewQueueItems));
    const QJsonObject body = SyncPlay::newQueueBody(itemIds.mid(start, SyncPlay::MaxNewQueueItems),
                                                    index - start, startTicks);
    if (!SyncPlay::isValidRequest(SyncPlay::Route::SetNewQueue, body))
        return false;
    sendIn(m_queueRequest, SyncPlay::Route::SetNewQueue, body);
    return true;
}

bool SyncPlayController::hasNext() const
{
    const qsizetype size = m_queue.entries.size();
    return m_queue.playingIndex >= 0
           && (m_queue.playingIndex + 1 < size
               || (m_queue.repeatMode == QLatin1String("RepeatAll") && size > 1));
}

bool SyncPlayController::hasPrevious() const
{
    return m_queue.playingIndex > 0
           || (m_queue.playingIndex == 0 && m_queue.repeatMode == QLatin1String("RepeatAll")
               && m_queue.entries.size() > 1);
}

bool SyncPlayController::requestNext()
{
    if (!routesPlayback() || m_entry.isEmpty() || !hasNext())
        return false;
    sendIn(m_stepRequest, SyncPlay::Route::NextItem, SyncPlay::playlistItemBody(m_entry));
    return true;
}

bool SyncPlayController::requestPrevious()
{
    if (!routesPlayback() || m_entry.isEmpty() || !hasPrevious())
        return false;
    sendIn(m_stepRequest, SyncPlay::Route::PreviousItem, SyncPlay::playlistItemBody(m_entry));
    return true;
}

bool SyncPlayController::itemEnded(const QString &itemId)
{
    if (!routesPlayback() || m_entry.isEmpty() || itemId != m_entryItem)
        return false;
    // Every member asks; the server moves once (observed). At the end of the queue nothing comes back, under a repeat it wraps (Repeat All observed)
    sendIn(m_stepRequest, SyncPlay::Route::NextItem, SyncPlay::playlistItemBody(m_entry));
    return hasNext() || m_queue.repeatMode != QLatin1String("RepeatNone");
}

bool SyncPlayController::shuffle() const
{
    return m_queue.shuffleMode == QLatin1String("Shuffle");
}

int SyncPlayController::repeatMode() const
{
    if (m_queue.repeatMode == QLatin1String("RepeatAll"))
        return 1;
    if (m_queue.repeatMode == QLatin1String("RepeatOne"))
        return 2;
    return 0;
}

void SyncPlayController::requestShuffle(bool shuffle)
{
    sendQueueChange(SyncPlay::Route::SetShuffleMode, SyncPlay::shuffleBody(shuffle));
}

void SyncPlayController::requestRepeatMode(int mode)
{
    static const QStringList modes{QStringLiteral("RepeatNone"), QStringLiteral("RepeatAll"),
                                   QStringLiteral("RepeatOne")};
    if (mode >= 0 && mode < modes.size())
        sendQueueChange(SyncPlay::Route::SetRepeatMode, SyncPlay::repeatBody(modes.at(mode)));
}

bool SyncPlayController::queueItems(const QStringList &itemIds, bool next)
{
    return sendQueueChange(SyncPlay::Route::Queue, SyncPlay::queueBody(itemIds, next));
}

bool SyncPlayController::removeEntry(const QString &playlistItemId)
{
    return sendQueueChange(SyncPlay::Route::RemoveFromPlaylist,
                           SyncPlay::removeBody({playlistItemId}, false, false));
}

bool SyncPlayController::moveEntry(const QString &playlistItemId, int newIndex)
{
    const int last = int(m_queue.entries.size()) - 1;
    if (last < 0)
        return false;
    return sendQueueChange(SyncPlay::Route::MovePlaylistItem,
                           SyncPlay::moveBody(playlistItemId, std::clamp(newIndex, 0, last)));
}

bool SyncPlayController::playEntry(const QString &playlistItemId)
{
    return sendQueueChange(SyncPlay::Route::SetPlaylistItem,
                           SyncPlay::playlistItemBody(playlistItemId));
}

bool SyncPlayController::sendQueueChange(SyncPlay::Route route, const QJsonObject &body)
{
    if (m_status != Status::Joined || m_offline || m_rejoining
        || !SyncPlay::isValidRequest(route, body))
        return false;
    return m_client.postSyncPlay(route, body);
}

void SyncPlayController::sendIn(RequestSlot &slot, SyncPlay::Route route, const QJsonObject &body)
{
    if (slot.inFlight) {
        slot.pending = std::make_pair(route, body);
        return;
    }
    slot.inFlight = m_client.postSyncPlay(route, body);
}

void SyncPlayController::onRequestFinished(SyncPlay::Route route, bool accepted, int httpStatus)
{
    if (m_status == Status::Joining && !accepted
        && (route == SyncPlay::Route::New || route == SyncPlay::Route::Join)) {
        qCInfo(syncPlayLog) << "the server refused the group request with status" << httpStatus;
        fail(route == SyncPlay::Route::New ? Failure::CreateDenied : Failure::JoinDenied, false);
        return;
    }
    if (!accepted && m_status == Status::Joined && (isPlayState(route) || isQueueRoute(route)))
        Q_EMIT failed(Failure::RequestFailed);
    RequestSlot *slot = nullptr;
    if (isPlayState(route))
        slot = &m_playState;
    else if (route == SyncPlay::Route::SetNewQueue)
        slot = &m_queueRequest;
    else if (route == SyncPlay::Route::NextItem || route == SyncPlay::Route::PreviousItem)
        slot = &m_stepRequest;
    if (!slot)
        return;
    slot->inFlight = false;
    // A session that stopped following, or lost its socket, no longer steers the group
    if (!routesPlayback())
        slot->pending.reset();
    if (slot->pending) {
        const auto [next, body] = *std::exchange(slot->pending, std::nullopt);
        sendIn(*slot, next, body);
    }
}

void SyncPlayController::updateAvailability()
{
    const QString access = m_client.syncPlayAccess();
    const bool available = m_client.isAuthenticated()
                           && m_socket.state() == JellyfinSocketClient::State::Authenticated
                           && access != QLatin1String("None");
    const bool canCreate = available && access != QLatin1String("JoinGroups");
    if (available == m_available && canCreate == m_canCreate)
        return;
    m_available = available;
    m_canCreate = canCreate;
    Q_EMIT availabilityChanged();
}

} // namespace Kinefin
