// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace Kinefin {

class PlaybackSession final {
  public:
    enum class State { Idle, Opening, Playing, Paused, Buffering, Stopping, Failed };
    enum class Route { Direct, Transcoded, Offline, Cast };

    // Called after every transition that happened, reset() included, with the state it left and the one it entered. Refused calls are silent. The identity is already cleared when a stopped() or reset() lands in Idle, and still set when stop() lands in Stopping. Observers are called in the order they were added and must not call back into the session or add or remove observers
    using Observer = std::function<void(State from, State to)>;

    struct Identity {
        std::string itemId;
        std::string mediaSourceId;
        std::string playSessionId;
    };

    PlaybackSession() = default;
    PlaybackSession(const PlaybackSession &) = delete;
    PlaybackSession &operator=(const PlaybackSession &) = delete;

    using ObserverId = std::size_t;

    ObserverId addObserver(Observer observer);
    void removeObserver(ObserverId id);

    bool open(Identity identity, Route route);
    bool opened();
    bool pause();
    bool resume();
    bool buffering();
    bool bufferRecovered();
    bool stop();
    bool stopped();
    // Refused while Stopping: a failure of the teardown must not start a second one, or the server would get two Stopped reports
    bool failed();
    // Clears the local model only. Call stop()/stopped() first for normal teardown; reset() is an explicit emergency escape hatch for an already reconciled session
    void reset();

    [[nodiscard]] State state() const { return m_state; }
    [[nodiscard]] Route route() const { return m_route; }
    [[nodiscard]] const Identity &identity() const { return m_identity; }
    [[nodiscard]] bool shouldReportToJellyfin() const;

  private:
    void transition(State to);

    std::vector<std::pair<ObserverId, Observer>> m_observers;
    ObserverId m_nextObserverId = 1;
    State m_state = State::Idle;
    Route m_route = Route::Direct;
    Identity m_identity;
    bool m_onlineSessionStarted = false;
};

} // namespace Kinefin
