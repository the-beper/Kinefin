// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ItemSummary.h"

#include "api/MediaSourceSelection.h"

#include <QJsonArray>
#include <QJsonValue>

namespace Kinefin {
namespace {
constexpr int kMaxNameLength = 512;
constexpr int kMaxTokenLength = 64;

QString cappedString(const QJsonValue &value, int limit)
{
    return value.isString() ? value.toString().left(limit) : QString();
}

int boundedInt(const QJsonValue &value, int minimum, int maximum, int fallback)
{
    if (!value.isDouble())
        return fallback;
    const double number = value.toDouble();
    return number < minimum || number > maximum ? fallback : static_cast<int>(number);
}
} // namespace

ItemSummary parseItemSummary(const QJsonObject &item)
{
    ItemSummary summary;
    const QString id = cappedString(item.value(QStringLiteral("Id")), kMaxTokenLength);
    summary.valid = isValidJellyfinId(id);
    if (!summary.valid)
        return summary;
    summary.id = id;
    summary.name = cappedString(item.value(QStringLiteral("Name")), kMaxNameLength);
    summary.type = cappedString(item.value(QStringLiteral("Type")), kMaxTokenLength);
    summary.year = boundedInt(item.value(QStringLiteral("ProductionYear")), 1, 9999, 0);
    summary.seriesName = cappedString(item.value(QStringLiteral("SeriesName")), kMaxNameLength);
    const QString seriesId = cappedString(item.value(QStringLiteral("SeriesId")), kMaxTokenLength);
    if (isValidJellyfinId(seriesId))
        summary.seriesId = seriesId;
    summary.seriesThumbTag =
        cappedString(item.value(QStringLiteral("SeriesThumbImageTag")), kMaxTokenLength);
    const QString parentBackdropItem =
        cappedString(item.value(QStringLiteral("ParentBackdropItemId")), kMaxTokenLength);
    if (isValidJellyfinId(parentBackdropItem)) {
        summary.parentBackdropItemId = parentBackdropItem;
        const QJsonValue parentTags = item.value(QStringLiteral("ParentBackdropImageTags"));
        if (parentTags.isArray() && !parentTags.toArray().isEmpty())
            summary.parentBackdropTag = cappedString(parentTags.toArray().first(), kMaxTokenLength);
    }
    summary.seasonNumber = boundedInt(item.value(QStringLiteral("ParentIndexNumber")), 0, 9999, -1);
    summary.episodeNumber = boundedInt(item.value(QStringLiteral("IndexNumber")), 0, 99999, -1);
    const QJsonValue tags = item.value(QStringLiteral("ImageTags"));
    if (tags.isObject()) {
        const QJsonObject tagObject = tags.toObject();
        summary.primaryTag =
            cappedString(tagObject.value(QStringLiteral("Primary")), kMaxTokenLength);
        summary.thumbTag = cappedString(tagObject.value(QStringLiteral("Thumb")), kMaxTokenLength);
    }
    const QJsonValue backdrops = item.value(QStringLiteral("BackdropImageTags"));
    if (backdrops.isArray() && !backdrops.toArray().isEmpty())
        summary.backdropTag = cappedString(backdrops.toArray().first(), kMaxTokenLength);
    const QJsonValue data = item.value(QStringLiteral("UserData"));
    if (data.isObject()) {
        const QJsonObject userData = data.toObject();
        summary.played = userData.value(QStringLiteral("Played")).toBool(false);
        summary.favorite = userData.value(QStringLiteral("IsFavorite")).toBool(false);
        summary.unplayedCount =
            boundedInt(userData.value(QStringLiteral("UnplayedItemCount")), 0, 1'000'000, -1);
        const QJsonValue percentage = userData.value(QStringLiteral("PlayedPercentage"));
        if (percentage.isDouble() && percentage.toDouble() >= 0 && percentage.toDouble() <= 100)
            summary.playedPercentage = percentage.toDouble();
    }
    return summary;
}
} // namespace Kinefin
