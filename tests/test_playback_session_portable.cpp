// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// No Qt headers: this can be compiled with a C++20 host even before KDE SDK exists
#include "player/PlaybackSession.h"

#undef NDEBUG // the asserts are the test
#include <cassert>
#include <iostream>
#include <utility>
#include <vector>

using Kinefin::PlaybackSession;

int main()
{
    PlaybackSession session;
    assert(!session.open({}, PlaybackSession::Route::Direct));
    assert(!session.shouldReportToJellyfin());
    assert(session.open({"item", "source", "session"}, PlaybackSession::Route::Direct));
    assert(!session.open({"another", "source", "session"}, PlaybackSession::Route::Direct));
    assert(session.state() == PlaybackSession::State::Opening);
    assert(!session.shouldReportToJellyfin());
    assert(session.opened());
    assert(session.shouldReportToJellyfin());
    assert(session.buffering());
    assert(session.bufferRecovered());
    assert(session.pause());
    assert(session.resume());
    assert(session.stop());
    assert(!session.stop());
    assert(session.stopped());
    assert(session.state() == PlaybackSession::State::Idle);
    assert(!session.shouldReportToJellyfin());
    assert(session.identity().itemId.empty());

    for (const auto route : {PlaybackSession::Route::Offline, PlaybackSession::Route::Cast}) {
        assert(session.open({"item", "source", ""}, route));
        assert(session.opened());
        assert(!session.shouldReportToJellyfin());
        assert(session.pause());
        assert(!session.shouldReportToJellyfin());
        assert(session.stop());
        assert(session.stopped());
    }
    assert(session.open({"item", "source", "session"}, PlaybackSession::Route::Transcoded));
    assert(session.failed());
    // Failure during opening has no active server session, but it is stoppable
    assert(session.stop());
    assert(!session.shouldReportToJellyfin());
    assert(!session.stop());
    assert(session.stopped());
    assert(session.identity().itemId.empty());
    assert(session.open({"another", "source", "session"}, PlaybackSession::Route::Direct));
    assert(session.opened());
    assert(session.failed());
    assert(!session.shouldReportToJellyfin());
    assert(session.stop());
    assert(!session.failed()); // a failure while Stopping must not start a second teardown
    assert(session.shouldReportToJellyfin()); // best-effort server stop due
    assert(session.stopped());
    assert(!session.shouldReportToJellyfin());
    assert(session.open({"third", "source", "session"}, PlaybackSession::Route::Direct));
    assert(session.stop()); // cancellation before first frame
    assert(!session.shouldReportToJellyfin());
    assert(session.stopped());

    using State = PlaybackSession::State;
    std::vector<std::pair<State, State>> seen;
    bool reportableAtPlaying = false;
    session.addObserver([&](State from, State to) {
        seen.emplace_back(from, to);
        if (from == State::Opening && to == State::Playing) {
            reportableAtPlaying = session.shouldReportToJellyfin();
        }
    });
    assert(!session.pause()); // refused calls are silent
    assert(seen.empty());
    assert(session.open({"item", "source", "session"}, PlaybackSession::Route::Direct));
    assert(session.opened());
    assert(session.pause());
    assert(session.stop());
    assert(session.stopped());
    const std::vector<std::pair<State, State>> expected{{State::Idle, State::Opening},
                                                        {State::Opening, State::Playing},
                                                        {State::Playing, State::Paused},
                                                        {State::Paused, State::Stopping},
                                                        {State::Stopping, State::Idle}};
    assert(seen == expected);
    assert(reportableAtPlaying);
    seen.clear();
    session.reset(); // already idle: nothing to notify
    assert(seen.empty());
    std::cout << "playback session state machine: PASS\n";
}
