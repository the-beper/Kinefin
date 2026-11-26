// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QList>
#include <QLoggingCategory>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVariant>
#include <QVariantMap>

#include <optional>

#include "api/MediaSourceSelection.h"
#include "player/HdrPolicy.h"
#include "player/PlaybackSession.h"

class MpvController;
class MpvAbstractItem;

Q_DECLARE_LOGGING_CATEGORY(playerLog)

namespace Kinefin {
class JellyfinClient;

// The video track `restoreVideo` selects again: `previous` (the last one mpv had selected) when the track list still has it as a video track, else the track marked default, else the first one, album art never being picked by the fallback; empty when the file has no such track
std::optional<qint64> videoTrackToRestore(const QVariantList &trackList,
                                          std::optional<qint64> previous);

// The mpv side of the one PlaybackSession: it opens a negotiated `PlaybackSource` through MpvQt's `MpvController` and turns mpv lifecycle events into calls of the session's state machine. It owns no state logic: `PlaybackSession` stays the only place transitions are decided, and reporting keeps its own cadence (`PlaybackReporter` observes the session; this class only feeds the probe through its accessors). The server URL never carries the access token: every open places the full `Authorization: MediaBrowser Token="…"` value as one mpv http-header-fields entry (the header mpv's ffmpeg path applies to every request of the stream, HLS segments included; verified against 12.4.0). The property must be set through an mpv node array of one entry: libmpv 0.41 would slice the API string form on every comma of the header (verified offline)
//
// The event mapping (`--vo=null`/`--ao=null` in the test container works the same as the windowed route):
//   `fileStarted`       → sends a stop() that arrived before the load began
//   `fileLoaded`        → session.opened() (the stream started decoding; reporting Start)
//   `endFile("stop")`   → session.stopped() when a stop() drive is outstanding, else session.stop() then stopped()
//   `endFile("eof")`    → session.stop() then stopped(): a Stopped report that marks the item's end at the duration
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
    // Bind to the renderer-owned mpv instance on its worker thread. MpvAbstractItem
    // forwards all mutations to that thread; never make direct calls on its controller.
    bool attach(MpvAbstractItem *item, MpvController *controller);
    void detach(MpvAbstractItem *item, bool stopSession = true);
    void setMpvProperty(const QString &name, const QVariant &value);
    QVariant mpvProperty(const QString &name) const;
    // Returns false when mpv rejects the command (or is unavailable).
    bool mpvCommand(const QStringList &arguments);
    [[nodiscard]] bool isInitialized() const { return m_controller != nullptr; }

    // Opens one negotiated source: the session first (Idle → Opening), then mpv's loadfile; the header lands before the load. A synchronous loadfile refusal is an opening failure: session.failed() and straight back to Idle (no server session was ever built to close). Asynchronous failures surface through `streamEnded("error")` and the session machine. Refuses a busy session. `startPaused` loads the file paused and the session goes on to Paused once it opened (a watching-together group starts everyone at once)
    bool open(const PlaybackSource &source, bool startPaused = false);

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
    // A seek was asked of mpv and its playback has not restarted yet (mpv's playback-restart event, or a watchdog when it never comes)
    [[nodiscard]] bool seeking() const { return m_seekPending; }
    // Whether the file's last time-pos is within a few seconds of its duration: the rule that makes an eof the item's end and not a dropped connection; valid in streamEnded() until the next open()
    [[nodiscard]] bool nearEnd() const;
    // The file counts as played to its end: the position reads as the duration until mpv says otherwise, so the report of the stop that follows closes it as the natural end would
    void markEnded();
    // mpv's own picture facts, cached from its property changes (no mpv call): `videoWidth`, `videoHeight` (dwidth, dheight), `droppedFrames` (frame-drop-count), `decoderDroppedFrames` (decoder-frame-drop-count), `hwdec` (hwdec-current); a key is present only while mpv has a value
    [[nodiscard]] QVariantMap videoFacts() const { return m_videoFacts; }
    // The colour facts of mpv's `video-params` (gamma, primaries and the HDR metadata), cached from its property changes: kept apart from `videoFacts`, which Playback Info shows whole; a fact mpv does not have reads empty or 0. They stay while the property is unavailable (the video is off during a renderer handover) and go with the file, a new open or a detach
    [[nodiscard]] HdrVideoFacts hdrVideoFacts() const { return m_hdrVideoFacts; }

    // The render context of the video item was replaced (the item moved to another window): libmpv turns the video off when the old context goes, so a loaded file whose `vid` is off gets its video track selected again. Nothing else changes: session, position and reports stay untouched. During the opening it waits for fileLoaded, where `afterLoad` lets it pick a track although none was selected yet
    void restoreVideo(bool afterLoad = false);

    // The controller, for the windowed item and the tests only: the engine owns it, and callers should stay in the engine API for everything but attaching renderers
    [[nodiscard]] MpvController *controller() const { return m_controller; }

  Q_SIGNALS:
    void positionChanged(qint64 ticks);
    void durationChanged(qint64 ticks);
    void canSeekChanged(bool seekable);
    void rateChanged(qreal rate);
    // mpv's end-file reason unchanged ("eof", "stop", "error"): a literal of mpv's own vocabulary, never a server-built string, so nothing secret can ride along
    void streamEnded(const QString &reason);
    void fileLoaded();
    // The decoder's picture parameters (video-dec-params) changed or appeared
    void videoParamsChanged();
    // The colour facts (`hdrVideoFacts`) changed, appeared or went with the file
    void hdrVideoFactsChanged();
    // The size mpv shows the video at (dwidth, dheight) changed, appeared or went with the file
    void videoSizeChanged();
    void seekingChanged(bool seeking);

  private:
    void applyStreamAuthorization();
    void wireController();
    QVariant command(const QStringList &arguments);
    void setProperty(const QString &name, const QVariant &value);
    // The terminal wrap-up mpv's own teardown cannot finish: a Failed session goes through the ordinary Stopping → Idle so the reporter sends its one best-effort Stopped report (or nothing, for an opening failure)
    void settleFailure();
    void sendStop();
    void onFileStarted();
    void onPropertyChanged(const QString &property, const QVariant &value);
    void onEndFile(const QString &reason);
    void clearVideoFacts();
    void setHdrVideoFacts(const HdrVideoFacts &facts);
    void readVideoSize();
    void settleSeek();
    void onPlaybackRestart();

    JellyfinClient &m_client;
    PlaybackSession &m_session;
    MpvController *m_controller = nullptr;
    MpvAbstractItem *m_renderer = nullptr;
    bool m_standalone = false;
    qint64 m_positionTicks = 0;
    bool m_positionKnown = false;
    bool m_loadStarted = false;
    bool m_stopPending = false;
    QTimer m_stopWatchdog;
    bool m_startPaused = false;
    // mpv's last paused-for-cache: a resume while the cache still holds the playback is a stall again
    bool m_cacheStalled = false;
    bool m_seekPending = false;
    qint64 m_seekTargetTicks = 0;
    QTimer m_seekWatchdog;
    qint64 m_durationTicks = 0;
    bool m_canSeek = false;
    qreal m_rate = 1.0;
    QVariantMap m_videoFacts;
    HdrVideoFacts m_hdrVideoFacts;
    // The last video track mpv reported as selected: `vid` reads off once the render context is gone
    std::optional<qint64> m_videoTrackId;
    // A renderer change came while the file opened: the video is chosen again at fileLoaded
    bool m_restorePending = false;
};
} // namespace Kinefin
