// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonObject>
#include <QString>
#include <optional>

#include "api/MediaSourceSelection.h"

namespace Kinefin {
// One playback report as the player sees it; the wire shape is parity §9 (Start, Progress and Stopped bodies of `POST /Sessions/Playing...`).
struct PlaybackReport {
    enum class Kind { Start, Progress, Stopped };

    Kind kind = Kind::Progress;
    QString itemId;
    QString mediaSourceId;
    QString playSessionId;
    // The client chooses and reports it; the server only records it.
    PlaybackSource::Method method = PlaybackSource::Method::Direct;
    // 100 ns ticks, never negative: a Stopped report without a position would mark the item as played.
    qint64 positionTicks = 0;
    bool isPaused = false;
    bool canSeek = true;
    // Stopped only: the playback ended in an error, the server leaves the user's data alone.
    bool failed = false;
};

// The JSON body of the report, or nullopt for an invalid id or a negative position. Fields that this slice cannot fill correctly are omitted on purpose (parity §9): the server owns `Item` and `SessionId`, track indexes wait for the mpv track mapping.
[[nodiscard]] std::optional<QJsonObject> playbackReportBody(const PlaybackReport &report);

// Body of `POST /Sessions/Capabilities/Full`. No `DeviceProfile`: the PlaybackInfo request is its only owner. The WebSocket component flips `SupportsMediaControl` and its caller is responsible for re-posting on every socket entry (`SupportedCommands` stays empty until the player names the commands it honours).
[[nodiscard]] QJsonObject clientCapabilitiesBody(bool mediaControlSupported = false);
} // namespace Kinefin
