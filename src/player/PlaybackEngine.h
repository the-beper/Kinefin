// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QList>
#include <QLoggingCategory>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVariant>

#include <optional>

#include "api/MediaSourceSelection.h"
#include "player/PlaybackSession.h"

class MpvController;

Q_DECLARE_LOGGING_CATEGORY(playerLog)

namespace Kinefin {
class JellyfinClient;

// The mpv side of the one PlaybackSession: it opens a negotiated `PlaybackSource` through MpvQt's `MpvController` and turns mpv lifecycle events into calls of the session's state machine. It owns no state logic: `PlaybackSession` stays the only place transitions are decided, and reporting keeps parity §9's cadence (`PlaybackReporter` observes the session; this class only feeds the probe through its accessors). The server URL never carries the access token: every open places the full `Authorization: MediaBrowser Token="…"` value as one mpv http-header-fields entry (the header mpv's ffmpeg path applies to every request of the stream, HLS segments included; verified against 12.4.0, api-notes.md). The property must be set through an mpv node array of one entry: libmpv 0.41 would slice the API string form on every comma of the header (verified offline)
//
// The event mapping (`--vo=null`/`--ao=null` in the test container works the same as the windowed route):
//   `fileStarted`       → sends a stop() that arrived before the load began
//   `fileLoaded`        → session.opened() (the stream started decoding; reporting Start)
//   `endFile("stop")`   → session.stopped() when a stop() drive is outstanding, else session.stop() then stopped()
//   `endFile("eof")`    → session.stop() then stopped(): a Stopped report that marks the item's end at the duration, parity §9
//   `endFile("error")`  → session.failed(), then the ordinary teardown: a failed open never reports a server session; a failed stream still gets its single best-effort Stopped report
//   `paused-for-cache`  → session.buffering()/bufferRecovered() from mpv's cache stalls
// Position, duration and playback rate are exposed both as accessors (the probe) and as QML-facing signals for the player window; rate ticks come straight from mpv's speed
class PlaybackEngine final : public QObject {
    Q_OBJECT

  public:
    // The client is read per open (its latest `streamAuthorization()`); the session ownership rules stay with the coordinator that created both
    PlaybackEngine(JellyfinClient &client, PlaybackSession &session, QObject *parent = nullptr);
    ~PlaybackEngine() override;

    // One-time muster: builds the mpv handle and the property observations. `options` are name=value mpv options set after MpvQt's own defaults (vo=libmpv there), e.g. "vo=null" and "ao=null" for the offscreen test container. Refuses a second init; a static libmpv that cannot be created would abort inside mpvqt
    bool init(const QStringList &options = QStringList());
    [[nodiscard]] bool isInitialized() const { return m_controller != nullptr; }

    // Opens one negotiated source: the session first (Idle → Opening), then mpv's loadfile; the header lands before the load. A synchronous loadfile refusal is an opening failure: session.failed() and straight back to Idle (no server session was ever built to close). Asynchronous failures surface through `streamEnded("error")` and the session machine. Refuses a busy session
    bool open(const PlaybackSource &source);

    // The calls run through the session first: a refusal stays a refusal, nothing is sent to mpv. idle/failed/stopping sessions receive nothing. stop() always leaves the session Stopping: it reaches Idle on mpv's end-file, and open() refuses until then
    void pause();
    void resume();
    void stop();

    // Playback rate and absolute seek; mpv clamps and answers with its own property change, which updates the cached rate the accessor reports
    void setRate(qreal rate);
    void seekTo(qint64 ticks);

    // The probe values: ticks (100 ns as everywhere in Kinefin) from mpv's seconds, updated by mpv's own property changes and good for a whole reporter cycle
    [[nodiscard]] qint64 positionTicks() const { return m_positionTicks; }
    // Empty from open() until mpv's first time-pos of that file: the reporter's probe must use this one, or an early report would carry 0 or the previous item's position
    [[nodiscard]] std::optional<qint64> knownPositionTicks() const
    {
        return m_positionKnown ? std::optional<qint64>(m_positionTicks) : std::nullopt;
    }
    [[nodiscard]] qint64 durationTicks() const { return m_durationTicks; }
    [[nodiscard]] bool canSeek() const { return m_canSeek; }
    [[nodiscard]] qreal playbackRate() const { return m_rate; }

    // The controller, for the windowed item and the tests only: the engine owns it, and callers should stay in the engine API for everything but attaching renderers
    [[nodiscard]] MpvController *controller() const { return m_controller; }

  Q_SIGNALS:
    void positionChanged(qint64 ticks);
    void durationChanged(qint64 ticks);
    void canSeekChanged(bool seekable);
    void rateChanged(qreal rate);
    // mpv's end-file reason unchanged ("eof", "stop", "error"): a literal of mpv's own vocabulary, never a server-built string, so nothing secret can ride along
    void streamEnded(const QString &reason);

  private:
    void applyStreamAuthorization();
    // The terminal wrap-up mpv's own teardown cannot finish: a Failed session goes through the ordinary Stopping → Idle so the reporter sends its one best-effort Stopped report (or nothing, for an opening failure)
    void settleFailure();
    void sendStop();
    void onFileStarted();
    void onPropertyChanged(const QString &property, const QVariant &value);
    void onEndFile(const QString &reason);

    JellyfinClient &m_client;
    PlaybackSession &m_session;
    MpvController *m_controller = nullptr;
    qint64 m_positionTicks = 0;
    bool m_positionKnown = false;
    bool m_loadStarted = false;
    bool m_stopPending = false;
    QTimer m_stopWatchdog;
    qint64 m_durationTicks = 0;
    bool m_canSeek = false;
    qreal m_rate = 1.0;
};
} // namespace Kinefin
