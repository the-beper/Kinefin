// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "api/PlaybackReport.h"

#include <QJsonArray>

namespace Kinefin {
std::optional<QJsonObject> playbackReportBody(const PlaybackReport &report)
{
    if (!isValidJellyfinId(report.itemId) || !isValidJellyfinId(report.mediaSourceId)
        || !isValidJellyfinId(report.playSessionId) || report.positionTicks < 0)
        return std::nullopt;
    QJsonObject body{{QStringLiteral("ItemId"), report.itemId},
                     {QStringLiteral("MediaSourceId"), report.mediaSourceId},
                     {QStringLiteral("PlaySessionId"), report.playSessionId},
                     {QStringLiteral("PositionTicks"), report.positionTicks}};
    if (report.audioStreamIndex && *report.audioStreamIndex >= 0)
        body.insert(QStringLiteral("AudioStreamIndex"), *report.audioStreamIndex);
    if (report.subtitleStreamIndex && *report.subtitleStreamIndex >= -1)
        body.insert(QStringLiteral("SubtitleStreamIndex"), *report.subtitleStreamIndex);
    if (report.volumeLevel && *report.volumeLevel >= 0 && *report.volumeLevel <= 100)
        body.insert(QStringLiteral("VolumeLevel"), *report.volumeLevel);
    if (report.muted)
        body.insert(QStringLiteral("IsMuted"), *report.muted);
    if (!report.playlistItemId.isEmpty() && isValidJellyfinId(report.playlistItemId))
        body.insert(QStringLiteral("PlaylistItemId"), report.playlistItemId);
    if (report.repeatMode >= 0 && report.repeatMode <= 2) {
        const QStringList tokens{QStringLiteral("RepeatNone"), QStringLiteral("RepeatAll"),
                                 QStringLiteral("RepeatOne")};
        body.insert(QStringLiteral("RepeatMode"), tokens.at(report.repeatMode));
    }
    if (report.shuffle)
        body.insert(QStringLiteral("PlaybackOrder"),
                    *report.shuffle ? QStringLiteral("Shuffle") : QStringLiteral("Default"));
    if (!report.nowPlayingQueue.isEmpty()) {
        QJsonArray queue;
        for (const QString &id : report.nowPlayingQueue) {
            if (!isValidJellyfinId(id) || queue.size() == 500)
                break;
            queue.append(QJsonObject{{QStringLiteral("Id"), id}});
        }
        if (!queue.isEmpty())
            body.insert(QStringLiteral("NowPlayingQueue"), queue);
    }
    if (report.kind == PlaybackReport::Kind::Stopped) {
        body.insert(QStringLiteral("Failed"), report.failed);
        return body;
    }
    body.insert(QStringLiteral("IsPaused"), report.isPaused);
    body.insert(QStringLiteral("CanSeek"), report.canSeek);
    // DirectStream is never reported: the media source selection never picks it
    body.insert(QStringLiteral("PlayMethod"), report.method == PlaybackSource::Method::Direct
                                                  ? QStringLiteral("DirectPlay")
                                                  : QStringLiteral("Transcode"));
    return body;
}

QJsonObject clientCapabilitiesBody(bool mediaControlSupported)
{
    return {{QStringLiteral("PlayableMediaTypes"), QJsonArray{QStringLiteral("Video")}},
            {QStringLiteral("SupportedCommands"), QJsonArray()},
            {QStringLiteral("SupportsMediaControl"), mediaControlSupported},
            {QStringLiteral("SupportsPersistentIdentifier"), true}};
}
} // namespace Kinefin
