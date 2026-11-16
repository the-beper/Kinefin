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

bool isPlayState(SyncPlay::Route route)
{
    return route == SyncPlay::Route::Pause || route == SyncPlay::Route::Unpause
           || route == SyncPlay::Route::Seek;
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
      m_firstSamplesMs(FirstSamplesMs), m_laterSamplesMs(LaterSamplesMs), m_groups(&client)
{
    connect(&m_socket, &JellyfinSocketClient::stateChanged, this,
            &SyncPlayController::updateAvailability);
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
    connect(&m_player, &PlayerController::changed, this, &SyncPlayController::sendReadyWhenSettled);
    updateAvailability();
}

SyncPlayController::~SyncPlayController()
{
    // The player outlives this object in main.cpp: it must not route to it afterwards
    m_player.setGroupPlayback(nullptr);
}

void SyncPlayController::setTestTimings(int joinTimeoutMs, int firstSamplesMs, int laterSamplesMs)
{
    if (joinTimeoutMs > 0)
        m_joinTimer.setInterval(joinTimeoutMs);
    if (firstSamplesMs > 0)
        m_firstSamplesMs = firstSamplesMs;
    if (laterSamplesMs > 0)
        m_laterSamplesMs = laterSamplesMs;
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
    if (leave && m_client.isAuthenticated())
        m_client.postSyncPlay(SyncPlay::Route::Leave, {});
    m_status = Status::Idle;
    // What plays goes on as the user's own
    m_player.releaseGroupItem();
    m_joiningGroupId.clear();
    m_groupId.clear();
    m_group = {};
    m_groupState = SyncPlay::GroupState::Idle;
    m_entry.clear();
    m_entryItem.clear();
    m_lastQueueUpdateMs = 0;
    m_readyWanted = false;
    m_lastCommand.reset();
    m_heldCommands.clear();
    m_heldExpiry.stop();
    m_scheduled.reset();
    m_commandTimer.stop();
    m_joinTimer.stop();
    m_sampleTimer.stop();
    m_jumpCheck.stop();
    m_sampleSentMs = -1;
    m_clock.reset();
    m_playState = {};
    m_queueRequest = {};
    Q_EMIT statusChanged();
    Q_EMIT groupChanged();
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
        sendReadyWhenSettled();
        Q_EMIT statusChanged();
        Q_EMIT groupChanged();
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
        }
        return;
    case Type::PlayQueue:
        if (ours)
            onQueue(update.queue);
        return;
    case Type::NotInGroup:
        // While joining it may answer an older request; once joined the server has forgotten this session
        if (m_status == Status::Joined)
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
        if (m_status == Status::Joining)
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
    if (queue.playingIndex < 0) {
        m_scheduled.reset();
        m_commandTimer.stop();
        const bool playing = !m_entryItem.isEmpty() && m_player.itemId() == m_entryItem;
        m_entry.clear();
        m_entryItem.clear();
        m_readyWanted = false;
        if (playing)
            m_player.applyStop();
        return;
    }
    const SyncPlay::QueueEntry &entry = queue.entries.at(queue.playingIndex);
    // A queue edit that keeps the playing entry, already open here, changes nothing for the player
    if (entry.playlistItemId == m_entry && m_player.itemId() == entry.itemId)
        return;
    m_scheduled.reset();
    m_commandTimer.stop();
    m_entry = entry.playlistItemId;
    m_entryItem = entry.itemId;
    m_readyWanted = m_player.openGroupItem(entry.itemId, queue.startPositionTicks);
    if (m_readyWanted)
        Q_EMIT itemOpened();
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
        // Not a moment to wait for: also the one that follows every join, which stops what played alone
        m_scheduled.reset();
        m_commandTimer.stop();
        m_readyWanted = false;
        if (m_player.active())
            m_player.applyStop();
        return;
    }
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
    const qint64 serverNow = m_clock.serverNowMs().value_or(0);
    if (serverNow - command.emittedAtMs > StaleCommandMs) {
        qCDebug(syncPlayLog) << "a stale command" << int(command.kind) << "is dropped";
        return;
    }
    if (command.playlistItemId.isEmpty() || command.playlistItemId != m_entry
        || !m_player.groupItemLoaded(m_entryItem)) {
        qCDebug(syncPlayLog) << "a command" << int(command.kind)
                             << "for an entry not loaded here is dropped";
        return;
    }
    m_scheduled.reset();
    m_commandTimer.stop();
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
        m_player.applyPause();
        if (command.positionTicks >= 0
            && std::llabs(position - command.positionTicks) > PositionToleranceTicks)
            m_player.applySeek(command.positionTicks);
        return;
    }
    // An unpause whose moment has passed plays from where the group is by now
    if (command.positionTicks >= 0) {
        const qint64 late =
            std::max<qint64>(0, m_clock.serverNowMs().value_or(command.whenMs) - command.whenMs);
        const qint64 target = command.positionTicks + late * TicksPerMs;
        if (std::llabs(position - target) > PositionToleranceTicks)
            m_player.applySeek(target);
    }
    m_player.applyUnpause();
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
    // The server times its commands by the slowest member's round trip
    if (m_status == Status::Joined)
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

void SyncPlayController::sendReadyWhenSettled()
{
    if (m_status != Status::Joined || !m_readyWanted || !m_clock.hasSample()
        || !m_player.groupItemSettled(m_entryItem))
        return;
    m_readyWanted = false;
    m_client.postSyncPlay(SyncPlay::Route::Ready,
                          SyncPlay::readinessBody(m_clock.serverNowMs().value_or(0),
                                                  m_player.positionTicks(), !m_player.paused(),
                                                  m_entry));
}

void SyncPlayController::requestPause()
{
    if (m_status == Status::Joined)
        sendIn(m_playState, SyncPlay::Route::Pause, {});
}

void SyncPlayController::requestUnpause()
{
    if (m_status == Status::Joined)
        sendIn(m_playState, SyncPlay::Route::Unpause, {});
}

void SyncPlayController::requestSeek(qint64 ticks)
{
    if (m_status == Status::Joined)
        sendIn(m_playState, SyncPlay::Route::Seek,
               SyncPlay::seekBody(std::clamp<qint64>(ticks, 0, SyncPlay::MaxPositionTicks)));
}

bool SyncPlayController::requestQueue(const QStringList &itemIds, int index, qint64 startTicks)
{
    if (m_status != Status::Joined || index < 0 || index >= itemIds.size())
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
    RequestSlot *slot = isPlayState(route)                      ? &m_playState
                        : route == SyncPlay::Route::SetNewQueue ? &m_queueRequest
                                                                : nullptr;
    if (!slot)
        return;
    slot->inFlight = false;
    if (!accepted && m_status == Status::Joined)
        Q_EMIT failed(Failure::RequestFailed);
    if (slot->pending && m_status == Status::Joined) {
        const auto [next, body] = *std::exchange(slot->pending, std::nullopt);
        sendIn(*slot, next, body);
    }
}

} // namespace Kinefin
