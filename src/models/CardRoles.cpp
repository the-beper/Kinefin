// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "CardRoles.h"

#include "api/JellyfinClient.h"

#include <QDateTime>

#include <algorithm>

namespace Kinefin {
QHash<int, QByteArray> cardRoleNames()
{
    return {{SeriesIdRole, "seriesId"},
            {RatingRole, "rating"},
            {RemainingMinutesRole, "remainingMinutes"},
            {IsNewRole, "isNew"},
            {SeasonCountRole, "seasonCount"},
            {EpisodeCountRole, "episodeCount"},
            {SeriesProgressRole, "seriesProgress"},
            {ResumeTicksRole, "resumeTicks"},
            {VirtualItemRole, "virtualItem"}};
}

QVariant cardRoleData(const ItemSummary &item, int role)
{
    const bool series = item.type == QLatin1String("Series");
    switch (role) {
    case SeriesIdRole:
        return item.seriesId;
    case RatingRole:
        return item.communityRating;
    case RemainingMinutesRole:
        return item.remainingMinutes;
    case IsNewRole:
        return isRecentlyAdded(item, QDateTime::currentDateTimeUtc());
    case SeasonCountRole:
        return series ? item.childCount : -1;
    case EpisodeCountRole:
        return series ? item.recursiveItemCount : -1;
    case ResumeTicksRole:
        return item.positionTicks;
    case VirtualItemRole:
        // A season of a series without season folders is Virtual too
        return item.virtualItem && item.type == QLatin1String("Episode");
    case SeriesProgressRole: {
        // May disagree with what the user has watched for virtual or missing episodes and for Season 0: the counts are the server's own
        if (!series || item.recursiveItemCount <= 0 || item.unplayedCount < 0)
            return qreal(-1);
        const int watched = std::max(0, item.recursiveItemCount - item.unplayedCount);
        return qreal(watched) / item.recursiveItemCount;
    }
    default:
        return {};
    }
}

QUrl cardImageUrl(JellyfinClient *client, const ItemSummary &item, bool landscape, int width)
{
    if (!client)
        return {};
    const auto url = [&](const QString &id, const QString &type, const QString &tag) {
        return id.isEmpty() || tag.isEmpty() ? QUrl() : client->imageUrl(id, type, width, tag);
    };
    if (landscape) {
        const QList<QUrl> candidates =
            item.type == QLatin1String("Episode")
                ? QList<QUrl>{url(item.parentThumbItemId, QStringLiteral("Thumb"),
                                  item.parentThumbTag),
                              url(item.seriesId, QStringLiteral("Thumb"), item.seriesThumbTag),
                              url(item.parentBackdropItemId, QStringLiteral("Backdrop"),
                                  item.parentBackdropTag)}
                : QList<QUrl>{url(item.id, QStringLiteral("Thumb"), item.thumbTag),
                              url(item.id, QStringLiteral("Backdrop"), item.backdropTag)};
        for (const QUrl &candidate : candidates) {
            if (!candidate.isEmpty())
                return candidate;
        }
    }
    if (item.type == QLatin1String("Season") && item.primaryTag.isEmpty()
        && !item.seriesId.isEmpty())
        return url(item.seriesId, QStringLiteral("Primary"), item.seriesPrimaryTag);
    return url(item.id, QStringLiteral("Primary"), item.primaryTag);
}
} // namespace Kinefin
