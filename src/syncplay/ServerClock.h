// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QList>
#include <QtGlobal>

#include <functional>
#include <optional>

namespace Kinefin {

// The server's clock as seen from here, from `GET /GetUtcTime` exchanges (NTP style): offset ((t1 − t0) + (t2 − t3)) / 2 and round trip (t3 − t0) − (t2 − t1), where t0 and t3 are local monotonic ms around the request and t1, t2 the server's reception and transmission times. Of the last MaxSamples exchanges the one with the lowest round trip wins, the newest on a tie. Scheduling uses the monotonic clock only; the wall clock serves to notice a suspend or a clock step, after which the samples no longer hold
class ServerClock final {
  public:
    static constexpr int MaxSamples = 8;
    // A gap between the wall and the monotonic clock that moved by more than this since the samples were taken (ms)
    static constexpr qint64 JumpToleranceMs = 2000;

    using Clock = std::function<qint64()>;
    // Both clocks in ms; the defaults are the process's monotonic clock and the system's wall clock
    explicit ServerClock(Clock monotonicMs = {}, Clock wallMs = {});

    qint64 monotonicNowMs() const;
    // false, changing nothing, for an impossible exchange (the reply before the request, the server's transmission before its reception, or a round trip more than 1 ms shorter than the server's own time: within it both clocks' ms truncation explains it, and the round trip counts as 0). A suspend or a clock step since the last sample drops the older samples first
    bool addSample(qint64 sentMonotonicMs, qint64 serverReceptionMs, qint64 serverTransmissionMs,
                   qint64 receivedMonotonicMs);
    bool hasSample() const;
    int sampleCount() const;
    // nullopt without a sample
    std::optional<qint64> serverNowMs() const;
    std::optional<qint64> monotonicForServerMs(qint64 serverMs) const;
    std::optional<qint64> roundTripMs() const;
    // Forgets the samples when the wall clock moved apart from the monotonic one by more than JumpToleranceMs since the last sample (a suspend, a clock step); true when it did
    bool dropAfterJump();
    void reset();

  private:
    struct Sample {
        qint64 offsetMs;
        qint64 roundTripMs;
    };
    std::optional<Sample> best() const;

    Clock m_monotonicMs;
    Clock m_wallMs;
    QList<Sample> m_samples;
    qint64 m_wallMinusMonotonicMs = 0;
};

} // namespace Kinefin
