// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <optional>

#include "api/MediaSourceSelection.h"

namespace Kinefin {
// One playback report as the player sees it; the wire shape of the Start, Progress and Stopped bodies of `POST /Sessions/Playing...`
struct PlaybackReport {
    enum class Kind { Start, Progress, Stopped };

    Kind kind = Kind::Progress;
    QString itemId;
    QString mediaSourceId;
    QString playSessionId;
    // The client chooses and reports it; the server only records it
    PlaybackSource::Method method = PlaybackSource::Method::Direct;
    // 100 ns ticks, never negative: a Stopped report without a position would mark the item as played
    qint64 positionTicks = 0;
    bool isPaused = false;
    bool canSeek = true;
    // Stopped only: the playback ended in an error, the server leaves the user's data alone
    bool failed = false;
    // Optional player-derived values. Never invent stream indexes when mpv's
    // mapping to the container stream is unknown.
    std::optional<int> audioStreamIndex;
    std::optional<int> subtitleStreamIndex;
    std::optional<int> volumeLevel;
    std::optional<bool> muted;
    // The play queue is a snapshot of authenticated, validated Jellyfin ids.
    QStringList nowPlayingQueue;
    QString playlistItemId;
    // -1 means unavailable, otherwise 0=off, 1=all, 2=one.
    int repeatMode = -1;
    std::optional<bool> shuffle;
};

// The JSON body of the report, or nullopt for an invalid id or a negative position. Fields the player cannot fill correctly are omitted on purpose: the server owns `Item` and `SessionId`, track indexes wait for the mpv track mapping
[[nodiscard]] std::optional<QJsonObject> playbackReportBody(const PlaybackReport &report);

// The `GeneralCommand` names the app honours from another session (the one list the capabilities announce and the remote command router accepts); `DisplayMessage` is left out on purpose, so no remote text reaches the screen
[[nodiscard]] const QStringList &supportedGeneralCommands();

// Body of `POST /Sessions/Capabilities/Full`. No `DeviceProfile`: the PlaybackInfo request is its only owner. The WebSocket component flips `SupportsMediaControl` and its caller is responsible for re-posting on every socket entry; `SupportedCommands` lists supportedGeneralCommands() only while media control is announced (the commands arrive over the socket)
[[nodiscard]] QJsonObject clientCapabilitiesBody(bool mediaControlSupported = false);
} // namespace Kinefin
