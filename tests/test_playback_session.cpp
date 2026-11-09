// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <QtTest>
#include "player/PlaybackSession.h"

using Kinefin::PlaybackSession;

class TestPlaybackSession : public QObject {
    Q_OBJECT
  private Q_SLOTS:
    void transitions()
    {
        PlaybackSession session;
        QVERIFY(!session.open({}, PlaybackSession::Route::Direct));
        QVERIFY(session.open({"item1", "source1", "session1"}, PlaybackSession::Route::Direct));
        QVERIFY(!session.open({"item2", "source2", "session2"}, PlaybackSession::Route::Direct));
        QVERIFY(!session.pause());
        QVERIFY(!session.shouldReportToJellyfin());
        QVERIFY(session.opened());
        QVERIFY(session.shouldReportToJellyfin());
        QVERIFY(session.buffering());
        QVERIFY(session.bufferRecovered());
        QVERIFY(session.pause());
        QVERIFY(session.resume());
        QVERIFY(session.stop());
        QVERIFY(!session.stop());
        QVERIFY(session.stopped());
        QCOMPARE(session.state(), PlaybackSession::State::Idle);
        QVERIFY(!session.shouldReportToJellyfin());
        QVERIFY(session.identity().itemId.empty());
    }
    void failureIsRefusedWhileStopping()
    {
        PlaybackSession session;
        QVERIFY(session.open({"item", "source", "session"}, PlaybackSession::Route::Direct));
        QVERIFY(session.opened());
        QVERIFY(session.stop());
        QVERIFY(!session.failed());
        QCOMPARE(session.state(), PlaybackSession::State::Stopping);
        QVERIFY(session.stopped());
    }
    void observersAreIndependent()
    {
        PlaybackSession session;
        int first = 0;
        int second = 0;
        const auto firstId = session.addObserver([&](auto, auto) { ++first; });
        const auto secondId = session.addObserver([&](auto, auto) { ++second; });
        QVERIFY(session.open({"item", "source", "session"}, PlaybackSession::Route::Direct));
        QCOMPARE(first, 1);
        QCOMPARE(second, 1);
        session.removeObserver(firstId);
        QVERIFY(session.opened());
        QCOMPARE(first, 1);
        QCOMPARE(second, 2);
        session.removeObserver(secondId);
        QVERIFY(session.stop());
        QCOMPARE(second, 2);
    }
    void observerSeesEveryTransition()
    {
        using State = PlaybackSession::State;
        PlaybackSession session;
        QList<QPair<State, State>> seen;
        bool reportableAtPlaying = false;
        bool identityAtStopping = false;
        bool identityClearedAtIdle = false;
        session.addObserver([&](State from, State to) {
            seen.append({from, to});
            if (to == State::Playing && from == State::Opening)
                reportableAtPlaying = session.shouldReportToJellyfin();
            if (to == State::Stopping)
                identityAtStopping = session.identity().itemId == "item";
            if (to == State::Idle)
                identityClearedAtIdle = session.identity().itemId.empty();
        });
        // Refused calls are silent.
        QVERIFY(!session.open({}, PlaybackSession::Route::Direct));
        QVERIFY(!session.pause());
        QVERIFY(seen.isEmpty());

        QVERIFY(session.open({"item", "source", "session"}, PlaybackSession::Route::Direct));
        QVERIFY(session.opened());
        QVERIFY(session.buffering());
        QVERIFY(session.bufferRecovered());
        QVERIFY(session.pause());
        QVERIFY(session.resume());
        QVERIFY(session.failed());
        QVERIFY(session.stop());
        QVERIFY(!session.stop());
        QVERIFY(session.stopped());
        const QList<QPair<State, State>> expected{
            {State::Idle, State::Opening},      {State::Opening, State::Playing},
            {State::Playing, State::Buffering}, {State::Buffering, State::Playing},
            {State::Playing, State::Paused},    {State::Paused, State::Playing},
            {State::Playing, State::Failed},    {State::Failed, State::Stopping},
            {State::Stopping, State::Idle}};
        QCOMPARE(seen, expected);
        QVERIFY(reportableAtPlaying);
        QVERIFY(identityAtStopping);
        QVERIFY(identityClearedAtIdle);

        // reset() notifies too, but not when already idle.
        seen.clear();
        session.reset();
        QVERIFY(seen.isEmpty());
        QVERIFY(session.open({"item", "source", "session"}, PlaybackSession::Route::Direct));
        session.reset();
        QCOMPARE(seen.size(), 2);
        QCOMPARE(seen.value(1), (QPair<State, State>{State::Opening, State::Idle}));
    }
    void offlineAndCastNeverReport()
    {
        for (auto route : {PlaybackSession::Route::Offline, PlaybackSession::Route::Cast}) {
            PlaybackSession session;
            QVERIFY(session.open({"item", "source", ""}, route));
            QVERIFY(session.opened());
            QVERIFY(!session.shouldReportToJellyfin());
            QVERIFY(session.stop());
            QVERIFY(!session.shouldReportToJellyfin());
        }
    }
    void failureRequiresExplicitCleanup()
    {
        PlaybackSession session;
        QVERIFY(session.open({"item", "source", "session"}, PlaybackSession::Route::Transcoded));
        QVERIFY(session.failed());
        QVERIFY(!session.opened());
        QVERIFY(!session.shouldReportToJellyfin());
        QVERIFY(!session.open({"item2", "source2", "session2"}, PlaybackSession::Route::Direct));
        QVERIFY(session.stop());
        QVERIFY(!session.stop());
        QVERIFY(!session.shouldReportToJellyfin()); // failure before stream opened
        QVERIFY(session.stopped());
        QVERIFY(session.open({"item2", "source2", "session2"}, PlaybackSession::Route::Direct));
        QVERIFY(session.opened());
        QVERIFY(session.failed());
        QVERIFY(!session.shouldReportToJellyfin()); // failed state suspends progress
        QVERIFY(session.stop());
        QVERIFY(session.shouldReportToJellyfin()); // remote cleanup is due
        QVERIFY(session.stopped());
        QVERIFY(!session.shouldReportToJellyfin());
        QVERIFY(session.identity().itemId.empty());
    }
    void cancellationBeforeOpening()
    {
        PlaybackSession session;
        QVERIFY(session.open({"item", "source", "session"}, PlaybackSession::Route::Direct));
        QVERIFY(session.stop());
        QVERIFY(!session.shouldReportToJellyfin());
        QVERIFY(session.stopped());
    }
};
QTEST_APPLESS_MAIN(TestPlaybackSession)
#include "test_playback_session.moc"
