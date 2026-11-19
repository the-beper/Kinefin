// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "player/BitrateFallback.h"

#include <algorithm>

namespace Kinefin {

namespace {
constexpr int EvaluateIntervalMs = 250;
}

std::optional<qint64> lowerBitrate(qint64 current)
{
    if (current <= 0)
        return std::nullopt;
    for (const qint64 step : FallbackLadder) {
        if (step * 10 <= current * 6)
            return step;
    }
    return std::nullopt;
}

std::optional<qint64> streamBitrate(qint64 transcodeTarget, qint64 sourceBitrate,
                                    qint64 requestedCap)
{
    if (transcodeTarget > 0)
        return transcodeTarget;
    const bool capped = requestedCap > 0 && requestedCap < UnlimitedBitrate;
    // A source under the cap plays at its own rate: the server sends it as it is
    if (capped && sourceBitrate > 0)
        return std::min(requestedCap, sourceBitrate);
    if (capped)
        return requestedCap;
    if (sourceBitrate > 0)
        return sourceBitrate;
    return std::nullopt;
}

BitrateFallback::BitrateFallback(QObject *parent) : QObject(parent)
{
    m_elapsed.start();
    m_clock = [this] { return m_elapsed.elapsed(); };
    m_timer.setInterval(EvaluateIntervalMs);
    connect(&m_timer, &QTimer::timeout, this, &BitrateFallback::evaluate);
}

void BitrateFallback::setTiming(const Timing &timing)
{
    m_timing = timing;
}

void BitrateFallback::setClock(std::function<qint64()> nowMs)
{
    m_clock = std::move(nowMs);
}

void BitrateFallback::clearHistory()
{
    m_closed.clear();
    if (m_openStart)
        m_openStart = now();
}

void BitrateFallback::reset()
{
    m_cap = UnlimitedBitrate;
    m_steps = 0;
    m_exhausted = false;
    m_pending = false;
    m_current.reset();
    m_ignoreUntil = 0;
    m_openStart.reset();
    m_closed.clear();
    m_timer.stop();
}

void BitrateFallback::streamLoaded(std::optional<qint64> currentBitrate, bool afterStep)
{
    m_current = currentBitrate;
    const qint64 t = now();
    m_ignoreUntil =
        t + (afterStep ? std::max(m_timing.graceMs, m_timing.cooldownMs) : m_timing.graceMs);
    clearHistory();
}

void BitrateFallback::stalled()
{
    if (m_openStart)
        return;
    m_openStart = now();
    m_timer.start();
}

void BitrateFallback::recovered()
{
    if (!m_openStart)
        return;
    const qint64 end = now();
    const qint64 start = std::max(*m_openStart, m_ignoreUntil);
    if (end > start)
        m_closed.append({start, end});
    // Many short stalls never reach the timer: the ones out of the window go here as well
    const qint64 windowStart = end - m_timing.windowMs;
    m_closed.removeIf([windowStart](const auto &stall) { return stall.second <= windowStart; });
    m_openStart.reset();
    m_timer.stop();
}

void BitrateFallback::restarted()
{
    const qint64 t = now();
    m_ignoreUntil = std::max(m_ignoreUntil, t + m_timing.graceMs);
    if (m_openStart)
        m_openStart = t;
}

void BitrateFallback::evaluate()
{
    if (m_pending || m_exhausted || m_steps >= MaxSteps || !m_current)
        return;
    const auto next = lowerBitrate(*m_current);
    if (!next)
        return;

    const qint64 t = now();
    const qint64 windowStart = t - m_timing.windowMs;
    m_closed.removeIf([windowStart](const auto &stall) { return stall.second <= windowStart; });

    qint64 counted = 0;
    for (const auto &[start, end] : std::as_const(m_closed))
        counted += end - std::max(start, windowStart);
    if (m_openStart) {
        const qint64 start = std::max({*m_openStart, m_ignoreUntil, windowStart});
        if (t > start)
            counted += t - start;
    }
    if (counted < m_timing.stallMs)
        return;

    m_pending = true;
    Q_EMIT starving(*next);
}

void BitrateFallback::stepTaken(qint64 cap)
{
    ++m_steps;
    m_cap = cap;
    m_pending = false;
    clearHistory();
}

void BitrateFallback::cancelPending()
{
    m_pending = false;
    clearHistory();
}

void BitrateFallback::exhaust()
{
    m_exhausted = true;
}

} // namespace Kinefin
