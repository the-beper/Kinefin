// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QStringList>
#include <QTimer>

#include <chrono>
#include <functional>
#include <optional>

#include "api/PlaybackReport.h"
#include "player/PlaybackSession.h"

namespace Kinefin {
class JellyfinClient;

// Turns the state of the one PlaybackSession into server reports (parity §9): Start when the stream opened, Progress every interval (also while paused, the server ends a silent session after five minutes) and at once on pause and resume, one Stopped when the session stops. It reads the session and the player, never changes them: a failed report does not touch playback
class PlaybackReporter final : public QObject {
    Q_OBJECT

  public:
    struct PlayerState {
        // 100 ns ticks; at the end of the file, the duration. Empty while the player does not know (no stream yet, unloaded, before the resume seek): the reporter then repeats the last known value and skips a Progress it has none for, because the server stores a reported 0 as the resume point
        std::optional<qint64> positionTicks{};
        // The route the state came from, when it is not the session's own (a cast whose source transcodes)
        std::optional<PlaybackSource::Method> method{};
        bool canSeek = true;
        std::optional<int> audioStreamIndex{};
        std::optional<int> subtitleStreamIndex{};
        std::optional<int> volume{};
        std::optional<bool> muted{};
        QStringList nowPlayingQueue{};
        QString playlistItemId{};
        int repeatMode = -1;
        std::optional<bool> shuffle{};
    };
    using PlayerProbe = std::function<PlayerState()>;

    // Must be created before the session opens and destroyed before the session and the client. The probe is called whenever a report is built, from inside the session's observer: it must not touch the session
    PlaybackReporter(PlaybackSession &session, JellyfinClient &client, PlayerProbe player,
                     std::chrono::milliseconds interval = std::chrono::seconds(10),
                     QObject *parent = nullptr);
    ~PlaybackReporter() override;

    // The player calls it after a seek or a track change: a Progress now, and the timer starts over
    void reportNow();

  private:
    void onTransition(PlaybackSession::State from, PlaybackSession::State to);
    void send(PlaybackReport::Kind kind);
    bool reportable() const;

    PlaybackSession &m_session;
    PlaybackSession::ObserverId m_observerId = 0;
    JellyfinClient &m_client;
    PlayerProbe m_player;
    QTimer m_timer;
    std::optional<qint64> m_lastPosition;
    bool m_failed = false;
};
} // namespace Kinefin
