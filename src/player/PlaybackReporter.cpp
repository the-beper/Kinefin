// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "player/PlaybackReporter.h"

#include "api/JellyfinClient.h"

#include <algorithm>

namespace Kinefin {
using State = PlaybackSession::State;

PlaybackReporter::PlaybackReporter(PlaybackSession &session, JellyfinClient &client,
                                   PlayerProbe player, std::chrono::milliseconds interval,
                                   QObject *parent)
    : QObject(parent), m_session(session), m_client(client), m_player(std::move(player))
{
    m_timer.setInterval(interval);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        if (reportable())
            send(PlaybackReport::Kind::Progress);
    });
    // Once the session is lost, or the user switched, signed out or removed, there is nobody to report to
    connect(&client, &JellyfinClient::sessionExpired, this, [this] { m_timer.stop(); });
    connect(&client, &JellyfinClient::credentialsCleared, this, [this] { m_timer.stop(); });
    m_observerId = session.addObserver([this](State from, State to) { onTransition(from, to); });
}

PlaybackReporter::~PlaybackReporter()
{
    m_session.removeObserver(m_observerId);
}

void PlaybackReporter::reportNow()
{
    if (!reportable())
        return;
    send(PlaybackReport::Kind::Progress);
    m_timer.start();
}

bool PlaybackReporter::reportable() const
{
    const State state = m_session.state();
    return m_session.shouldReportToJellyfin()
           && (state == State::Playing || state == State::Paused || state == State::Buffering);
}

void PlaybackReporter::onTransition(State from, State to)
{
    switch (to) {
    case State::Playing:
        if (from == State::Opening) {
            // The stream is open (an HLS job exists), so the server records the real PlayMethod
            if (m_session.shouldReportToJellyfin()) {
                send(PlaybackReport::Kind::Start);
                m_timer.start();
            }
        } else if (from == State::Paused && reportable()) {
            send(PlaybackReport::Kind::Progress);
            m_timer.start();
        }
        break;
    case State::Paused:
        if (reportable()) {
            send(PlaybackReport::Kind::Progress);
            m_timer.start();
        }
        break;
    case State::Failed:
        m_failed = true;
        m_timer.stop();
        break;
    case State::Stopping:
        m_timer.stop();
        if (m_session.shouldReportToJellyfin())
            send(PlaybackReport::Kind::Stopped);
        break;
    case State::Idle:
        m_timer.stop();
        m_failed = false;
        m_lastPosition.reset();
        break;
    case State::Opening:
    case State::Buffering:
        break;
    }
}

void PlaybackReporter::send(PlaybackReport::Kind kind)
{
    const PlaybackSession::Identity &identity = m_session.identity();
    const PlayerState player = m_player ? m_player() : PlayerState{};
    if (player.positionTicks)
        m_lastPosition = player.positionTicks;
    // Start and Stopped always go out (they open and close the server's session); a heartbeat without any position would reset the resume point
    if (kind == PlaybackReport::Kind::Progress && !m_lastPosition)
        return;
    PlaybackReport report;
    report.kind = kind;
    report.itemId = QString::fromStdString(identity.itemId);
    report.mediaSourceId = QString::fromStdString(identity.mediaSourceId);
    report.playSessionId = QString::fromStdString(identity.playSessionId);
    report.method = m_session.route() == PlaybackSession::Route::Transcoded
                        ? PlaybackSource::Method::Transcode
                        : PlaybackSource::Method::Direct;
    report.positionTicks = std::max<qint64>(0, m_lastPosition.value_or(0));
    report.canSeek = player.canSeek;
    report.isPaused = m_session.state() == State::Paused;
    report.failed = m_failed;
    report.audioStreamIndex = player.audioStreamIndex;
    report.subtitleStreamIndex = player.subtitleStreamIndex;
    report.volumeLevel = player.volume;
    report.muted = player.muted;
    report.nowPlayingQueue = player.nowPlayingQueue;
    report.playlistItemId = player.playlistItemId;
    report.repeatMode = player.repeatMode;
    report.shuffle = player.shuffle;
    // The result goes nowhere on purpose: playback never depends on a report
    m_client.reportPlayback(report);
}
} // namespace Kinefin
