// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The download speed: an average over a sliding window of samples, zero without two samples apart, falling while no sample arrives, and a counter that goes back starts it again
#include "downloads/SpeedAverage.h"

#include <QtTest/QtTest>

using Kinefin::SpeedAverage;

namespace {
// A transfer at 1 MB/s for 3 s, then 4 MB/s: bytes at `timeMs`
qint64 counter(qint64 timeMs)
{
    return timeMs <= 3000 ? timeMs * 1000 : 3'000'000 + (timeMs - 3000) * 4000;
}

// A sample every 250 ms, as the transfer reports them, from `from` to `to`
void feed(SpeedAverage *average, qint64 from, qint64 to)
{
    for (qint64 time = from; time <= to; time += 250)
        average->add(time, counter(time));
}
} // namespace

class TestSpeedAverage : public QObject {
    Q_OBJECT

  private Q_SLOTS:
    void readsZeroUntilTwoSamplesAreApart()
    {
        SpeedAverage average;
        QVERIFY(average.empty());
        QCOMPARE(average.bytesPerSecond(0), 0);
        average.add(1000, 500'000);
        QCOMPARE(average.bytesPerSecond(1000), 0);
        average.add(1100, 600'000);
        QCOMPARE(average.bytesPerSecond(1100), 0);
        average.add(1200, 700'000);
        QCOMPARE(average.bytesPerSecond(1200), 1'000'000);
    }

    void aSteadyTransferReadsItsRate()
    {
        SpeedAverage average;
        feed(&average, 0, 3000);
        QCOMPARE(average.bytesPerSecond(3000), 1'000'000);
    }

    void theWindowSlidesOverAChangeOfRate()
    {
        SpeedAverage average;
        feed(&average, 0, 4500);
        // 1.5 s at 1 MB/s and 1.5 s at 4 MB/s
        QCOMPARE(average.bytesPerSecond(4500), 2'500'000);
        feed(&average, 4750, 6000);
        QCOMPARE(average.bytesPerSecond(6000), 4'000'000);
    }

    void fallsToZeroWhileNoSampleArrives()
    {
        SpeedAverage average;
        feed(&average, 0, 6000);
        QCOMPARE(average.bytesPerSecond(6000), 4'000'000);
        // Nothing arrives after 6 s: the window moves on without it, so the average falls, and reads zero once a single sample is left
        const qint64 later = average.bytesPerSecond(7000);
        QVERIFY(later > 0 && later < 4'000'000);
        QVERIFY(average.bytesPerSecond(8000) < later);
        QCOMPARE(average.bytesPerSecond(9000), 0);
        QCOMPARE(average.bytesPerSecond(60'000), 0);
    }

    void aCounterThatGoesBackStartsAgain()
    {
        SpeedAverage average;
        feed(&average, 0, 3000);
        average.add(3250, 0);
        QCOMPARE(average.bytesPerSecond(3250), 0);
        average.add(3500, 250'000);
        QCOMPARE(average.bytesPerSecond(3500), 1'000'000);
    }

    void aClockThatGoesBackStartsAgain()
    {
        SpeedAverage average;
        feed(&average, 0, 3000);
        average.add(100, counter(3000) + 1000);
        QCOMPARE(average.bytesPerSecond(100), 0);
    }

    void clearForgetsTheSamples()
    {
        SpeedAverage average;
        feed(&average, 0, 3000);
        average.clear();
        QVERIFY(average.empty());
        QCOMPARE(average.bytesPerSecond(3000), 0);
    }
};

QTEST_APPLESS_MAIN(TestSpeedAverage)
#include "test_speed_average.moc"
