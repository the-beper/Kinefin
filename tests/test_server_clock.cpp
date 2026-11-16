// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The SyncPlay clock with injected monotonic and wall clocks: the offset and round trip of one exchange, the lowest round trip of the last eight winning, the impossible exchanges refused, and a suspend or clock step dropping the samples
#include "syncplay/ServerClock.h"

#include <QtTest>

using Kinefin::ServerClock;

namespace {
std::optional<qint64> ms(qint64 value)
{
    return value;
}
} // namespace

class TestServerClock : public QObject {
    Q_OBJECT

  private Q_SLOTS:
    void symmetricExchange()
    {
        qint64 monotonic = 1000;
        ServerClock clock([&] { return monotonic; }, [&] { return monotonic + 50'000; });
        QVERIFY(!clock.hasSample());
        // Sent at 1000, answered at 1100; the server stamped 6050 on both ends, half way
        QVERIFY(clock.addSample(1000, 6050, 6050, 1100));
        QVERIFY(clock.hasSample());
        QCOMPARE(clock.roundTripMs(), ms(100));
        monotonic = 1200;
        QCOMPARE(clock.serverNowMs(), ms(6200));
        QCOMPARE(clock.monotonicForServerMs(7000), ms(2000));
    }

    void theServersOwnTimeIsNotRoundTrip()
    {
        ServerClock clock([] { return 0; }, [] { return 0; });
        QVERIFY(clock.addSample(1000, 6040, 6060, 1100));
        QCOMPARE(clock.roundTripMs(), ms(80));
        QCOMPARE(clock.monotonicForServerMs(6050), ms(1050));
    }

    void theLowestRoundTripWins()
    {
        ServerClock clock([] { return 0; }, [] { return 0; });
        QVERIFY(clock.addSample(1000, 6050, 6050, 1100));
        // A slow exchange with an asymmetric path would move the offset: it loses
        QVERIFY(clock.addSample(2000, 7250, 7250, 2300));
        QCOMPARE(clock.roundTripMs(), ms(100));
        QCOMPARE(clock.monotonicForServerMs(6050), ms(1050));
        QVERIFY(clock.addSample(3000, 8010, 8010, 3020));
        QCOMPARE(clock.roundTripMs(), ms(20));
        QCOMPARE(clock.monotonicForServerMs(8010), ms(3010));
        QCOMPARE(clock.sampleCount(), 3);
    }

    void onlyTheLastEightCount()
    {
        ServerClock clock([] { return 0; }, [] { return 0; });
        QVERIFY(clock.addSample(0, 5005, 5005, 10));
        for (int i = 1; i <= ServerClock::MaxSamples; ++i)
            QVERIFY(clock.addSample(i * 1000, i * 1000 + 5100, i * 1000 + 5100, i * 1000 + 200));
        QCOMPARE(clock.sampleCount(), ServerClock::MaxSamples);
        QCOMPARE(clock.roundTripMs(), ms(200));
    }

    void impossibleExchangesAreRefused()
    {
        ServerClock clock([] { return 0; }, [] { return 0; });
        QVERIFY(!clock.addSample(1100, 6050, 6050, 1000));
        QVERIFY(!clock.addSample(1000, 6060, 6040, 1100));
        // The server claims more time than the whole exchange took
        QVERIFY(!clock.addSample(1000, 6000, 6200, 1100));
        QVERIFY(!clock.hasSample());
    }

    void aJumpDropsTheSamples()
    {
        qint64 monotonic = 1000;
        qint64 wall = 1'700'000'000'000;
        ServerClock clock([&] { return monotonic; }, [&] { return wall; });
        QVERIFY(clock.addSample(1000, 6050, 6050, 1100));
        // Both clocks move together, within the tolerance
        monotonic += 5000;
        wall += 5000 + ServerClock::JumpToleranceMs;
        QVERIFY(!clock.dropAfterJump());
        QVERIFY(clock.hasSample());
        // A suspend: the wall clock ran on while the monotonic one stood still
        wall += 60'000;
        QVERIFY(clock.dropAfterJump());
        QVERIFY(!clock.hasSample());
        QVERIFY(!clock.dropAfterJump());
        // The next exchange takes the new gap as its reference
        QVERIFY(clock.addSample(monotonic, 9000, 9000, monotonic + 10));
        QVERIFY(!clock.dropAfterJump());
    }

    void defaultClocksRun()
    {
        ServerClock clock;
        const qint64 first = clock.monotonicNowMs();
        QTest::qWait(20);
        QVERIFY(clock.monotonicNowMs() >= first + 10);
        QVERIFY(clock.addSample(first, 5000, 5000, clock.monotonicNowMs()));
        QVERIFY(!clock.dropAfterJump());
    }

    void anEmptyClockTellsNoTime()
    {
        ServerClock clock([] { return 0; }, [] { return 0; });
        QVERIFY(!clock.serverNowMs());
        QVERIFY(!clock.monotonicForServerMs(1000));
        QVERIFY(!clock.roundTripMs());
    }

    void millisecondTruncationIsTolerated()
    {
        ServerClock clock([] { return 0; }, [] { return 0; });
        // A sub-millisecond exchange: the monotonic ms did not move, the server's did
        QVERIFY(clock.addSample(1000, 6000, 6001, 1000));
        QCOMPARE(clock.roundTripMs(), ms(0));
        QVERIFY(!clock.addSample(1000, 6000, 6002, 1000));
        QCOMPARE(clock.sampleCount(), 1);
    }

    void theNewestWinsATie()
    {
        ServerClock clock([] { return 0; }, [] { return 0; });
        QVERIFY(clock.addSample(1000, 6050, 6050, 1100));
        QVERIFY(clock.addSample(2000, 7150, 7150, 2100));
        QCOMPARE(clock.monotonicForServerMs(7150), ms(2050));
    }

    void aSampleAfterAJumpDropsTheOlderOnes()
    {
        qint64 monotonic = 1000;
        qint64 wall = 1'700'000'000'000;
        ServerClock clock([&] { return monotonic; }, [&] { return wall; });
        QVERIFY(clock.addSample(1000, 6005, 6005, 1010));
        // Resumed from a suspend without anyone asking dropAfterJump(): the old, fast sample must not win
        wall += 60'000;
        monotonic += 100;
        QVERIFY(clock.addSample(1100, 66'200, 66'200, 1300));
        QCOMPARE(clock.sampleCount(), 1);
        QCOMPARE(clock.monotonicForServerMs(66'200), ms(1200));
    }

    void aSlowSlewNeverAddsUp()
    {
        qint64 monotonic = 0;
        qint64 wall = 1'700'000'000'000;
        ServerClock clock([&] { return monotonic; }, [&] { return wall; });
        for (int i = 0; i < 10; ++i) {
            monotonic += 10'000;
            wall += 10'000 + ServerClock::JumpToleranceMs / 2;
            QVERIFY(clock.addSample(monotonic, 5000 + monotonic, 5000 + monotonic, monotonic + 10));
            QVERIFY(!clock.dropAfterJump());
        }
        QCOMPARE(clock.sampleCount(), ServerClock::MaxSamples);
    }
};

QTEST_GUILESS_MAIN(TestServerClock)
#include "test_server_clock.moc"
