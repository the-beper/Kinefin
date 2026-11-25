// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "SpeedAverage.h"

#include <algorithm>

namespace Kinefin {
void SpeedAverage::add(qint64 timeMs, qint64 bytes)
{
    if (!m_samples.empty() && (timeMs < m_samples.back().timeMs || bytes < m_samples.back().bytes))
        m_samples.clear();
    m_samples.push_back({timeMs, bytes});
    while (m_samples.size() > MaxSamples || m_samples.front().timeMs < timeMs - WindowMs)
        m_samples.pop_front();
}

qint64 SpeedAverage::bytesPerSecond(qint64 nowMs) const
{
    for (const Sample &oldest : m_samples) {
        if (oldest.timeMs < nowMs - WindowMs)
            continue;
        const Sample &newest = m_samples.back();
        if (newest.timeMs - oldest.timeMs < MinSpanMs)
            return 0;
        const qint64 span = std::max(nowMs, newest.timeMs) - oldest.timeMs;
        return (newest.bytes - oldest.bytes) * 1000 / span;
    }
    return 0;
}
} // namespace Kinefin
