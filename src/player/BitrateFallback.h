// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QTimer>
#include <QtGlobal>

#include <array>
#include <functional>
#include <optional>
#include <utility>

namespace Kinefin {

// MaxStreamingBitrate steps in bit/s, highest first (jellyfin-web's quality values, inferred)
inline constexpr std::array<qint64, 11> FallbackLadder{
    60'000'000, 40'000'000, 20'000'000, 15'000'000, 10'000'000, 8'000'000,
    6'000'000,  4'000'000,  3'000'000,  2'000'000,  1'500'000};

// The client's default MaxStreamingBitrate, meaning no cap
constexpr qint64 UnlimitedBitrate = 1'000'000'000;

// The highest step at most 60 % of `current`; empty under the 1.5 Mbps floor or when `current` is not positive
std::optional<qint64> lowerBitrate(qint64 current);

// What a stream plays at: a positive transcode target wins, else the requested cap when positive and below the default, else a positive source bitrate; empty when unknown
std::optional<qint64> streamBitrate(qint64 transcodeTarget, qint64 sourceBitrate,
                                    qint64 requestedCap);

// The one owner of when to fall back to a lower bitrate and to what: it counts cache-stall time over a sliding window and emits `starving` once per decision, the controller owns how the switch is made
class BitrateFallback final : public QObject {
    Q_OBJECT
  public:
    struct Timing {
        int stallMs = 10'000;
        int windowMs = 60'000;
        int graceMs = 5'000;
        int cooldownMs = 30'000;
    };
    static constexpr int MaxSteps = 3;

    explicit BitrateFallback(QObject *parent = nullptr);

    void setTiming(const Timing &timing);
    // Tests drive the counting with a fake monotonic clock in milliseconds
    void setClock(std::function<qint64()> nowMs);

    // Full quality back: cap unlimited, no steps, not exhausted, nothing pending, history cleared
    void reset();
    // The stream that now plays and its bitrate (empty: unknown, never falls back); the grace starts now, and after a step the cooldown too
    void streamLoaded(std::optional<qint64> currentBitrate, bool afterStep);
    void stalled();
    // Also called when a stall ends in a pause or a stop
    void recovered();
    // A seek landed: the grace starts again and an open stall restarts its counting now
    void restarted();
    void evaluate();
    // The lower stream was accepted
    void stepTaken(qint64 cap);
    // The caller dropped the pending fallback (pause, item change): the budget is kept
    void cancelPending();
    // No more fallbacks for this item until reset()
    void exhaust();

    [[nodiscard]] qint64 cap() const { return m_cap; }
    [[nodiscard]] int steps() const { return m_steps; }
    [[nodiscard]] bool exhausted() const { return m_exhausted; }
    [[nodiscard]] bool pending() const { return m_pending; }

  Q_SIGNALS:
    void starving(qint64 nextCap);

  private:
    qint64 now() const { return m_clock(); }
    void clearHistory();

    Timing m_timing;
    std::function<qint64()> m_clock;
    QElapsedTimer m_elapsed;
    QTimer m_timer;
    qint64 m_cap = UnlimitedBitrate;
    int m_steps = 0;
    bool m_exhausted = false;
    bool m_pending = false;
    std::optional<qint64> m_current;
    // Stall time before this instant (grace, cooldown) is never counted
    qint64 m_ignoreUntil = 0;
    std::optional<qint64> m_openStart;
    // Closed stalls as {start, end}, already clipped at the ignore instant
    QList<std::pair<qint64, qint64>> m_closed;
};

} // namespace Kinefin
