// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArrayView>
#include <QJsonObject>
#include <QMetaType>
#include <QString>
#include <QStringView>
#include <QUrl>
#include <optional>

namespace Kinefin {
// What the player needs to open one item: the chosen media source and a media URL under the server's own base URL. The URL never carries the access token: the caller sends it in an `Authorization` header (api-notes.md, Media URLs).
struct PlaybackSource {
    enum class Method { Direct, Transcode };

    QString itemId;
    QString mediaSourceId;
    QString playSessionId;
    Method method = Method::Direct;
    QUrl url;
};

// Either a source or a short, fixed English reason; never a URL, a body or a path from the server. The reasons are internal: a UI that shows one maps it to a translated string.
struct SourceSelection {
    std::optional<PlaybackSource> source;
    QString failure;
};

// Item and media source ids are interpolated into URLs: only [A-Za-z0-9-], 1 to 64 characters.
[[nodiscard]] bool isValidJellyfinId(QStringView id);

// Picks the source of a `PlaybackInfoResponse` (parity §3, "Media-source selection"): the first source that direct-plays, else the first one with a safe `TranscodingUrl`. `serverBase` is the authenticated server URL (its path is kept, a server may live under a prefix); `accessToken` is only used to refuse any URL that would still carry it.
[[nodiscard]] SourceSelection selectPlaybackSource(const QJsonObject &response,
                                                   const QString &itemId, const QUrl &serverBase,
                                                   QByteArrayView accessToken);
} // namespace Kinefin

Q_DECLARE_METATYPE(Kinefin::PlaybackSource)
