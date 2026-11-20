// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ItemSummary.h"

#include "api/MediaSourceSelection.h"

#include <QJsonArray>
#include <QJsonValue>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>

namespace Kinefin {
bool sameItemId(const QString &a, const QString &b)
{
    const auto normalized = [](const QString &id) {
        return QString(id).remove(QLatin1Char('-')).toLower();
    };
    return !a.isEmpty() && normalized(a) == normalized(b);
}

namespace {
constexpr int kMaxNameLength = 512;
constexpr int kMaxTokenLength = 64;
constexpr int kMaxOverview = 2048;
constexpr int kMaxDateLength = 64;
constexpr int kMinYear = 1900;
constexpr int kMaxYear = 2200;
constexpr double kTicksPerMinute = 6e8;
constexpr double kMaxTicks = 1e14;

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

// A count is a whole number: 2.5 seasons is a malformed reply, not 2
int boundedCount(const QJsonValue &value, int maximum)
{
    if (!value.isDouble() || std::trunc(value.toDouble()) != value.toDouble())
        return -1;
    return boundedInt(value, 0, maximum, -1);
}

qreal parseCommunityRating(const QJsonValue &value)
{
    const double rating = value.toDouble(-1);
    return value.isDouble() && std::isfinite(rating) && rating >= 0 && rating <= 10 ? rating : -1;
}

// An ISO 8601 timestamp in UTC, invalid when it is too long, malformed or in an implausible year. The server writes seven fractional digits: the fraction is dropped before parsing, nothing here needs it
QDateTime parseTimestamp(const QJsonValue &value)
{
    if (!value.isString() || value.toString().size() > kMaxDateLength)
        return {};
    static const QRegularExpression fraction(QStringLiteral("\\.\\d+"));
    QString text = value.toString();
    text.remove(fraction);
    const QDateTime parsed = QDateTime::fromString(text, Qt::ISODate).toUTC();
    const int year = parsed.date().year();
    return parsed.isValid() && year >= kMinYear && year <= kMaxYear ? parsed : QDateTime();
}

// Minutes still to watch, rounded up; -1 unless the position lies strictly inside the runtime
int remainingMinutesOf(double positionTicks, double runtimeTicks)
{
    if (!(positionTicks > 0 && positionTicks < runtimeTicks && runtimeTicks < kMaxTicks))
        return -1;
    return int(std::ceil((runtimeTicks - positionTicks) / kTicksPerMinute));
}
} // namespace

QString overviewText(const QJsonValue &value, int limit)
{
    QString text = cappedString(value, limit);
    static const QRegularExpression lineBreak(QStringLiteral("<br\\s*/?>"),
                                              QRegularExpression::CaseInsensitiveOption);
    text.replace(lineBreak, QStringLiteral("\n"));
    return text;
}

void applyPlayed(ItemSummary &item, bool played)
{
    item.played = played;
    item.playedPercentage = 0;
    item.positionTicks = 0;
    item.remainingMinutes = -1;
}

void applyProgress(ItemSummary &item, qint64 positionTicks, double playedPercentage)
{
    constexpr double ticksPerMinute = 60.0 * 10'000'000;
    item.positionTicks = std::max<qint64>(0, positionTicks);
    const double runtimeTicks = item.runtimeMinutes * ticksPerMinute;
    // A server that sends no share leaves it to the runtime
    item.playedPercentage =
        playedPercentage > 0 || runtimeTicks <= 0
            ? std::clamp(playedPercentage, 0.0, 100.0)
            : std::clamp(100.0 * double(item.positionTicks) / runtimeTicks, 0.0, 100.0);
    item.remainingMinutes =
        item.runtimeMinutes > 0 && item.positionTicks > 0 && item.positionTicks < runtimeTicks
            ? int(std::ceil((runtimeTicks - double(item.positionTicks)) / ticksPerMinute))
            : -1;
}

bool isRecentlyAdded(const ItemSummary &item, const QDateTime &nowUtc)
{
    // A date more than a day ahead is a wrong clock or a hostile value: it never counts as new
    return !item.played && item.dateCreated.isValid()
           && item.dateCreated > nowUtc.addDays(-NewItemDays)
           && item.dateCreated <= nowUtc.addDays(1);
}

bool isPlayableVideo(const QString &type)
{
    return type == QLatin1String("Movie") || type == QLatin1String("Episode")
           || type == QLatin1String("Video");
}

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
    const QString parentThumbItem =
        cappedString(item.value(QStringLiteral("ParentThumbItemId")), kMaxTokenLength);
    if (isValidJellyfinId(parentThumbItem)) {
        summary.parentThumbItemId = parentThumbItem;
        summary.parentThumbTag =
            cappedString(item.value(QStringLiteral("ParentThumbImageTag")), kMaxTokenLength);
    }
    if (isValidJellyfinId(parentBackdropItem)) {
        summary.parentBackdropItemId = parentBackdropItem;
        const QJsonValue parentTags = item.value(QStringLiteral("ParentBackdropImageTags"));
        if (parentTags.isArray() && !parentTags.toArray().isEmpty())
            summary.parentBackdropTag = cappedString(parentTags.toArray().first(), kMaxTokenLength);
    }
    summary.seasonNumber = boundedInt(item.value(QStringLiteral("ParentIndexNumber")), 0, 9999, -1);
    summary.episodeNumber = boundedInt(item.value(QStringLiteral("IndexNumber")), 0, 99999, -1);
    summary.episodeNumberEnd =
        boundedInt(item.value(QStringLiteral("IndexNumberEnd")), 0, 99999, -1);
    // A season's own IndexNumber is its number: it is not an episode number
    if (summary.type == QLatin1String("Season")) {
        summary.seasonNumber = summary.episodeNumber;
        summary.episodeNumber = -1;
        summary.episodeNumberEnd = -1;
    }
    summary.seriesPrimaryTag =
        cappedString(item.value(QStringLiteral("SeriesPrimaryImageTag")), kMaxTokenLength);
    summary.overview = overviewText(item.value(QStringLiteral("Overview")), kMaxOverview);
    const double ticks = item.value(QStringLiteral("RunTimeTicks")).toDouble(0);
    if (ticks > 0 && ticks < kMaxTicks)
        summary.runtimeMinutes = int(std::max(1.0, ticks / kTicksPerMinute));
    summary.communityRating = parseCommunityRating(item.value(QStringLiteral("CommunityRating")));
    summary.dateCreated = parseTimestamp(item.value(QStringLiteral("DateCreated")));
    summary.childCount = boundedCount(item.value(QStringLiteral("ChildCount")), 1'000'000);
    summary.recursiveItemCount =
        boundedCount(item.value(QStringLiteral("RecursiveItemCount")), 10'000'000);
    const QJsonValue tags = item.value(QStringLiteral("ImageTags"));
    if (tags.isObject()) {
        const QJsonObject tagObject = tags.toObject();
        summary.primaryTag =
            cappedString(tagObject.value(QStringLiteral("Primary")), kMaxTokenLength);
        summary.thumbTag = cappedString(tagObject.value(QStringLiteral("Thumb")), kMaxTokenLength);
        summary.bannerTag =
            cappedString(tagObject.value(QStringLiteral("Banner")), kMaxTokenLength);
        summary.discTag = cappedString(tagObject.value(QStringLiteral("Disc")), kMaxTokenLength);
        summary.logoTag = cappedString(tagObject.value(QStringLiteral("Logo")), kMaxTokenLength);
    }
    const QJsonValue backdrops = item.value(QStringLiteral("BackdropImageTags"));
    if (backdrops.isArray() && !backdrops.toArray().isEmpty())
        summary.backdropTag = cappedString(backdrops.toArray().first(), kMaxTokenLength);
    summary.virtualItem =
        item.value(QStringLiteral("LocationType")).toString() == QLatin1String("Virtual");
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
        const double position = userData.value(QStringLiteral("PlaybackPositionTicks")).toDouble(0);
        summary.remainingMinutes = remainingMinutesOf(position, ticks);
        if (position > 0 && position < kMaxTicks)
            summary.positionTicks = qint64(position);
    }
    return summary;
}
} // namespace Kinefin
