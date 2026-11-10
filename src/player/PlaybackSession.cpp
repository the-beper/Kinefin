// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "PlaybackSession.h"

#include <algorithm>
#include <utility>

namespace Kinefin {

PlaybackSession::ObserverId PlaybackSession::addObserver(Observer observer)
{
    m_observers.emplace_back(m_nextObserverId, std::move(observer));
    return m_nextObserverId++;
}

void PlaybackSession::removeObserver(ObserverId id)
{
    std::erase_if(m_observers, [id](const auto &entry) { return entry.first == id; });
}

bool PlaybackSession::open(Identity identity, Route route)
{
    if (m_state != State::Idle || identity.itemId.empty()) {
        return false;
    }
    m_identity = std::move(identity);
    m_route = route;
    m_onlineSessionStarted = false;
    transition(State::Opening);
    return true;
}

bool PlaybackSession::opened()
{
    if (m_state != State::Opening) {
        return false;
    }
    // A stream was opened: an online route now has a server session to close. Set before the transition so the observer already sees it
    m_onlineSessionStarted = (m_route == Route::Direct || m_route == Route::Transcoded);
    transition(State::Playing);
    return true;
}

bool PlaybackSession::pause()
{
    if (m_state != State::Playing && m_state != State::Buffering) {
        return false;
    }
    transition(State::Paused);
    return true;
}

bool PlaybackSession::resume()
{
    if (m_state != State::Paused) {
        return false;
    }
    transition(State::Playing);
    return true;
}

bool PlaybackSession::buffering()
{
    if (m_state != State::Playing) {
        return false;
    }
    transition(State::Buffering);
    return true;
}

bool PlaybackSession::bufferRecovered()
{
    if (m_state != State::Buffering) {
        return false;
    }
    transition(State::Playing);
    return true;
}

bool PlaybackSession::stop()
{
    if (m_state == State::Idle || m_state == State::Stopping) {
        return false;
    }
    transition(State::Stopping);
    return true;
}

bool PlaybackSession::stopped()
{
    if (m_state != State::Stopping) {
        return false;
    }
    reset();
    return true;
}

bool PlaybackSession::failed()
{
    if (m_state == State::Idle || m_state == State::Failed || m_state == State::Stopping) {
        return false;
    }
    transition(State::Failed);
    return true;
}

void PlaybackSession::reset()
{
    m_route = Route::Direct;
    m_identity = {};
    m_onlineSessionStarted = false;
    transition(State::Idle);
}

void PlaybackSession::transition(State to)
{
    const State from = m_state;
    m_state = to;
    if (from == to) {
        return;
    }
    for (const auto &entry : m_observers) {
        entry.second(from, to);
    }
}

bool PlaybackSession::shouldReportToJellyfin() const
{
    // A remote stop remains eligible after Failed -> Stopping, but opening failures cannot report a phantom server session. Offline/cast never publish *local mpv* progress or a local stop. The adapter still owns network retries and must not confuse eligibility with delivery success
    return m_onlineSessionStarted
           && (m_state == State::Playing || m_state == State::Paused || m_state == State::Buffering
               || m_state == State::Stopping);
}

} // namespace Kinefin
