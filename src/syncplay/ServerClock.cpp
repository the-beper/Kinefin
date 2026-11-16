// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "syncplay/ServerClock.h"

#include <QDateTime>
#include <QDeadlineTimer>

#include <algorithm>
#include <cstdlib>

namespace Kinefin {

ServerClock::ServerClock(Clock monotonicMs, Clock wallMs)
    : m_monotonicMs(monotonicMs ? std::move(monotonicMs)
                                : [] { return QDeadlineTimer::current().deadline(); }),
      m_wallMs(wallMs ? std::move(wallMs) : [] { return QDateTime::currentMSecsSinceEpoch(); })
{
}

qint64 ServerClock::monotonicNowMs() const
{
    return m_monotonicMs();
}

bool ServerClock::addSample(qint64 sentMonotonicMs, qint64 serverReceptionMs,
                            qint64 serverTransmissionMs, qint64 receivedMonotonicMs)
{
    const qint64 serverTime = serverTransmissionMs - serverReceptionMs;
    const qint64 roundTrip = (receivedMonotonicMs - sentMonotonicMs) - serverTime;
    if (receivedMonotonicMs < sentMonotonicMs || serverTime < 0 || roundTrip < -1)
        return false;
    dropAfterJump();
    // The reference for the next jump check moves with every sample, so a slow slew of the wall clock never adds up to a jump
    m_wallMinusMonotonicMs = m_wallMs() - m_monotonicMs();
    const qint64 offset =
        ((serverReceptionMs - sentMonotonicMs) + (serverTransmissionMs - receivedMonotonicMs)) / 2;
    m_samples.append({offset, std::max<qint64>(roundTrip, 0)});
    if (m_samples.size() > MaxSamples)
        m_samples.removeFirst();
    return true;
}

bool ServerClock::hasSample() const
{
    return !m_samples.isEmpty();
}

int ServerClock::sampleCount() const
{
    return int(m_samples.size());
}

std::optional<ServerClock::Sample> ServerClock::best() const
{
    std::optional<Sample> best;
    for (const Sample &sample : m_samples) {
        if (!best || sample.roundTripMs <= best->roundTripMs)
            best = sample;
    }
    return best;
}

std::optional<qint64> ServerClock::serverNowMs() const
{
    const auto sample = best();
    if (!sample)
        return std::nullopt;
    return m_monotonicMs() + sample->offsetMs;
}

std::optional<qint64> ServerClock::monotonicForServerMs(qint64 serverMs) const
{
    const auto sample = best();
    if (!sample)
        return std::nullopt;
    return serverMs - sample->offsetMs;
}

std::optional<qint64> ServerClock::roundTripMs() const
{
    const auto sample = best();
    if (!sample)
        return std::nullopt;
    return sample->roundTripMs;
}

bool ServerClock::dropAfterJump()
{
    if (m_samples.isEmpty())
        return false;
    const qint64 gap = m_wallMs() - m_monotonicMs();
    if (std::llabs(gap - m_wallMinusMonotonicMs) <= JumpToleranceMs)
        return false;
    reset();
    return true;
}

void ServerClock::reset()
{
    m_samples.clear();
}

} // namespace Kinefin
