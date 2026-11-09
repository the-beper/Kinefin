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
    if (report.kind == PlaybackReport::Kind::Stopped) {
        body.insert(QStringLiteral("Failed"), report.failed);
        return body;
    }
    body.insert(QStringLiteral("IsPaused"), report.isPaused);
    body.insert(QStringLiteral("CanSeek"), report.canSeek);
    // DirectStream is never reported: the media source selection never picks it.
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
