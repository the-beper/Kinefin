// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QtGlobal>

#include <cstddef>
#include <deque>

namespace Kinefin {
// The rate of a growing byte counter, averaged over the samples of the last `WindowMs`: the oldest sample still in the window to the newest, divided by the time since the oldest. The caller supplies the times (milliseconds of a monotonic clock), so the class has no clock and needs no timer: it reads zero until two samples `MinSpanMs` apart are in the window, and falls toward zero while no sample arrives, because the window moves on without them
class SpeedAverage {
  public:
    static constexpr qint64 WindowMs = 3000;
    static constexpr qint64 MinSpanMs = 200;

    // A counter that goes back (a transfer that restarted from zero) starts the average again
    void add(qint64 timeMs, qint64 bytes);
    qint64 bytesPerSecond(qint64 nowMs) const;
    // The time of the newest sample; only meaningful when !empty()
    qint64 lastSampleMs() const { return m_samples.empty() ? 0 : m_samples.back().timeMs; }
    bool empty() const { return m_samples.empty(); }
    void clear() { m_samples.clear(); }

  private:
    struct Sample {
        qint64 timeMs = 0;
        qint64 bytes = 0;
    };
    // A sample every 250 ms fills the window with a dozen entries; the cap only bounds a caller that adds faster
    static constexpr std::size_t MaxSamples = 64;

    std::deque<Sample> m_samples;
};
} // namespace Kinefin
