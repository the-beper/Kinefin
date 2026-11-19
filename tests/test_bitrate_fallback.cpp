// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The fallback decision runs on a fake clock moved by the test: no sleeps, no event loop
#include "player/BitrateFallback.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

using namespace Kinefin;

namespace {
constexpr qint64 Source = 18'300'000;
}

class TestBitrateFallback : public QObject {
    Q_OBJECT

  private:
    qint64 m_now = 0;
    BitrateFallback m_fb;
    QSignalSpy m_spy{&m_fb, &BitrateFallback::starving};

  private Q_SLOTS:
    void init()
    {
        m_fb.reset();
        m_fb.setTiming({});
        m_now = 0;
        m_fb.setClock([this] { return m_now; });
        m_spy.clear();
        m_fb.streamLoaded(Source, false);
    }

    void ladderStepsSixtyPercentDown()
    {
        QCOMPARE(lowerBitrate(18'300'000), 10'000'000);
        QCOMPARE(lowerBitrate(10'000'000), 6'000'000);
        QCOMPARE(lowerBitrate(6'000'000), 3'000'000);
        QCOMPARE(lowerBitrate(100'000'000), 60'000'000);
        QCOMPARE(lowerBitrate(2'500'000), 1'500'000);
        QVERIFY(!lowerBitrate(2'499'999));
        QVERIFY(!lowerBitrate(1'500'000));
        QVERIFY(!lowerBitrate(1'000'000));
        QVERIFY(!lowerBitrate(0));
        QVERIFY(!lowerBitrate(-5));
    }

    void unknownBitrateNeverFallsBack()
    {
        m_fb.streamLoaded(std::nullopt, false);
        m_fb.stalled();
        m_now = 100'000;
        m_fb.evaluate();
        QCOMPARE(m_spy.count(), 0);
        QVERIFY(!m_fb.pending());
    }

    void streamBitrateFollowsTargetThenCapThenSource()
    {
        QCOMPARE(streamBitrate(5'000'000, 18'300'000, 8'000'000), 5'000'000);
        QCOMPARE(streamBitrate(5'000'000, 0, UnlimitedBitrate), 5'000'000);
        QCOMPARE(streamBitrate(0, 18'300'000, 8'000'000), 8'000'000);
        QCOMPARE(streamBitrate(-1, 18'300'000, 8'000'000), 8'000'000);
        QCOMPARE(streamBitrate(0, 18'300'000, UnlimitedBitrate), 18'300'000);
        // A direct play of a source under the cap plays at the source's rate
        QCOMPARE(streamBitrate(0, 5'000'000, 10'000'000), 5'000'000);
        QCOMPARE(streamBitrate(0, 0, 8'000'000), 8'000'000);
        QCOMPARE(streamBitrate(0, 18'300'000, 0), 18'300'000);
        QVERIFY(!streamBitrate(0, 0, UnlimitedBitrate));
        QVERIFY(!streamBitrate(0, -1, 0));
    }

    void oneLongStallStarves()
    {
        m_now = 5'000;
        m_fb.stalled();
        m_now = 14'999;
        m_fb.evaluate();
        QCOMPARE(m_spy.count(), 0);
        m_now = 15'000;
        m_fb.evaluate();
        QCOMPARE(m_spy.count(), 1);
        QCOMPARE(m_spy.at(0).at(0).toLongLong(), 10'000'000);
        QVERIFY(m_fb.pending());
        m_now = 30'000;
        m_fb.evaluate();
        QCOMPARE(m_spy.count(), 1);
    }

    void shortStallsAddUpInTheWindow()
    {
        qint64 start = 5'000;
        for (int i = 0; i < 8; ++i) {
            m_now = start;
            m_fb.stalled();
            m_now = start + 1'250;
            m_fb.evaluate();
            if (i < 7) {
                QCOMPARE(m_spy.count(), 0);
                m_fb.recovered();
            }
            start += 6'000;
        }
        QCOMPARE(m_spy.count(), 1);
        QVERIFY(m_now < 60'000);
    }

    void oldStallsLeaveTheWindow()
    {
        for (const qint64 start : {10'000, 20'000, 30'000}) {
            m_now = start;
            m_fb.stalled();
            m_now = start + 3'000;
            m_fb.recovered();
        }
        m_now = 80'000;
        m_fb.evaluate();
        m_fb.stalled();
        m_now = 83'000;
        m_fb.evaluate();
        QCOMPARE(m_spy.count(), 0);
    }

    void stallsInTheGraceAreNotCounted()
    {
        m_fb.stalled();
        m_now = 10'000;
        m_fb.evaluate();
        QCOMPARE(m_spy.count(), 0);
        m_now = 14'999;
        m_fb.evaluate();
        QCOMPARE(m_spy.count(), 0);
        m_now = 15'000;
        m_fb.evaluate();
        QCOMPARE(m_spy.count(), 1);

        m_fb.cancelPending();
        m_fb.recovered();
        m_spy.clear();
        m_now = 40'000;
        m_fb.stalled();
        m_now = 41'000;
        m_fb.restarted();
        m_now = 50'999;
        m_fb.evaluate();
        QCOMPARE(m_spy.count(), 0);
        m_now = 51'000;
        m_fb.evaluate();
        QCOMPARE(m_spy.count(), 0);
        m_now = 56'000;
        m_fb.evaluate();
        QCOMPARE(m_spy.count(), 1);
    }

    void timeOutsideAStallIsNotCounted()
    {
        m_now = 6'000;
        m_fb.stalled();
        m_now = 12'000;
        m_fb.recovered();
        m_now = 40'000;
        m_fb.evaluate();
        QCOMPARE(m_spy.count(), 0);
        m_fb.stalled();
        m_now = 43'999;
        m_fb.evaluate();
        QCOMPARE(m_spy.count(), 0);
        m_now = 44'000;
        m_fb.evaluate();
        QCOMPARE(m_spy.count(), 1);
    }

    void cooldownAfterAStep()
    {
        m_now = 5'000;
        m_fb.stalled();
        m_now = 15'000;
        m_fb.evaluate();
        QCOMPARE(m_spy.count(), 1);
        m_fb.stepTaken(10'000'000);
        QCOMPARE(m_fb.cap(), 10'000'000);
        QCOMPARE(m_fb.steps(), 1);
        QVERIFY(!m_fb.pending());

        m_now = 16'000;
        m_fb.streamLoaded(10'000'000, true);
        m_now = 45'999;
        m_fb.evaluate();
        m_now = 55'999;
        m_fb.evaluate();
        QCOMPARE(m_spy.count(), 1);
        m_now = 56'000;
        m_fb.evaluate();
        QCOMPARE(m_spy.count(), 2);
        QCOMPARE(m_spy.at(1).at(0).toLongLong(), 6'000'000);
    }

    void threeStepsAtMost()
    {
        qint64 bitrate = Source;
        for (int step = 0; step < BitrateFallback::MaxSteps; ++step) {
            m_fb.stalled();
            m_now += 40'000;
            m_fb.evaluate();
            QCOMPARE(m_spy.count(), step + 1);
            const qint64 next = m_spy.last().at(0).toLongLong();
            m_fb.recovered();
            m_fb.stepTaken(next);
            bitrate = next;
            m_fb.streamLoaded(bitrate, true);
        }
        QCOMPARE(m_fb.steps(), BitrateFallback::MaxSteps);
        QCOMPARE(bitrate, 3'000'000);
        m_fb.stalled();
        m_now += 200'000;
        m_fb.evaluate();
        QCOMPARE(m_spy.count(), BitrateFallback::MaxSteps);
    }

    void exhaustedStopsUntilReset()
    {
        m_fb.exhaust();
        QVERIFY(m_fb.exhausted());
        m_fb.stalled();
        m_now = 60'000;
        m_fb.evaluate();
        QCOMPARE(m_spy.count(), 0);
        m_fb.reset();
        QVERIFY(!m_fb.exhausted());
        m_fb.streamLoaded(Source, false);
        m_fb.stalled();
        m_now += 20'000;
        m_fb.evaluate();
        QCOMPARE(m_spy.count(), 1);
    }

    void pendingEmitsOnce()
    {
        m_fb.stalled();
        m_now = 20'000;
        m_fb.evaluate();
        QCOMPARE(m_spy.count(), 1);
        m_now = 40'000;
        m_fb.evaluate();
        QCOMPARE(m_spy.count(), 1);
        m_fb.stepTaken(10'000'000);
        m_fb.streamLoaded(10'000'000, true);
        m_now = 100'000;
        m_fb.evaluate();
        QCOMPARE(m_spy.count(), 2);
    }

    void resetRestoresFullQuality()
    {
        m_fb.stalled();
        m_now = 20'000;
        m_fb.evaluate();
        m_fb.stepTaken(10'000'000);
        m_fb.exhaust();
        m_fb.reset();
        QCOMPARE(m_fb.cap(), UnlimitedBitrate);
        QCOMPARE(m_fb.steps(), 0);
        QVERIFY(!m_fb.exhausted());
        QVERIFY(!m_fb.pending());
    }

    void cancelPendingKeepsTheBudget()
    {
        m_fb.stalled();
        m_now = 20'000;
        m_fb.evaluate();
        QVERIFY(m_fb.pending());
        m_fb.stepTaken(10'000'000);
        m_fb.streamLoaded(10'000'000, true);
        m_fb.stalled();
        m_now = 80'000;
        m_fb.evaluate();
        QCOMPARE(m_spy.count(), 2);
        QVERIFY(m_fb.pending());
        m_fb.cancelPending();
        QVERIFY(!m_fb.pending());
        QCOMPARE(m_fb.steps(), 1);
        QCOMPARE(m_fb.cap(), 10'000'000);
        // The history was cleared: a new decision needs a fresh 10 s
        m_now = 85'000;
        m_fb.evaluate();
        QCOMPARE(m_spy.count(), 2);
        m_now = 90'000;
        m_fb.evaluate();
        QCOMPARE(m_spy.count(), 3);
    }
};

QTEST_GUILESS_MAIN(TestBitrateFallback)
#include "test_bitrate_fallback.moc"
