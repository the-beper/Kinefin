// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArrayView>
#include <QJsonObject>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QStringView>
#include <QUrl>
#include <optional>

#include "models/ItemDetails.h"

namespace Kinefin {
// What the player needs to open one item: the chosen media source and a media URL under the server's own base URL. The URL never carries the access token: the caller sends it in an `Authorization` header (api-notes.md, Media URLs)
struct ExternalSubtitle {
    int index = -1;
    QString language;
    QString label;
    QString format;
    QUrl url;
};

// The facts of the chosen media source for the Playback Info overlay. Everything comes from the server and is untrusted: strings are capped plain tokens, numbers range-checked, and no URL or query value other than the ones below is kept
struct PlaybackMediaInfo {
    static constexpr int MaxTranscodeReasons = 16;

    // Container, size, bitrate and the streams, as the details page reads them
    SourceSummary source;
    // `File` for a direct URL, `HLS` for a playlist, else the container extension of the URL
    QString streamType;
    // The scheme of the media URL
    QString protocol;
    // First codec the TranscodingUrl asks for, empty for a direct play
    QString targetVideoCodec;
    QString targetAudioCodec;
    // `TranscodeReason` enum names (letters only), from the TranscodingUrl or the media source
    QStringList transcodeReasons;
    // The bit rate a transcode is made at, in bit/s: the TranscodingUrl's VideoBitrate plus AudioBitrate, each 1 kbit/s to 1 Gbit/s; 0 for a direct play or when unknown
    qint64 targetBitrate = 0;
};

struct PlaybackSource {
    enum class Method { Direct, Transcode, Local };

    QString itemId;
    QString mediaSourceId;
    QString playSessionId;
    Method method = Method::Direct;
    QUrl url;
    QList<ExternalSubtitle> externalSubtitles;
    PlaybackMediaInfo media;
};

// The stream of `type` (Video or Audio) with the server index `index` when given, else the default one, else the first
[[nodiscard]] const StreamSummary *pickStream(const SourceSummary &source, QStringView type,
                                              int index = -1);

// Either a source or a short, fixed English reason; never a URL, a body or a path from the server. The reasons are internal: a UI that shows one maps it to a translated string
struct SourceSelection {
    std::optional<PlaybackSource> source;
    QString failure;
};

// Item and media source ids are interpolated into URLs: only [A-Za-z0-9-], 1 to 64 characters
[[nodiscard]] bool isValidJellyfinId(QStringView id);

// Whether the URL holds the access token anywhere, encoded or decoded
[[nodiscard]] bool carriesToken(const QUrl &url, QByteArrayView accessToken);

// Picks the source of a `PlaybackInfoResponse` (parity §3, "Media-source selection"): the first source that direct-plays, else the first one with a safe `TranscodingUrl`. `serverBase` is the authenticated server URL (its path is kept, a server may live under a prefix); `accessToken` is only used to refuse any URL that would still carry it
[[nodiscard]] SourceSelection selectPlaybackSource(const QJsonObject &response,
                                                   const QString &itemId, const QUrl &serverBase,
                                                   QByteArrayView accessToken);
} // namespace Kinefin

Q_DECLARE_METATYPE(Kinefin::PlaybackSource)
