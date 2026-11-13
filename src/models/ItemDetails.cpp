// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ItemDetails.h"

#include "ItemSummary.h"

#include "api/MediaSourceSelection.h"

#include <QJsonArray>
#include <QJsonValue>

#include <algorithm>

namespace Kinefin {
namespace {
constexpr qint64 kMaxTicks = 100'000'000'000'000;
constexpr qint64 kTicksPerMinute = 600'000'000;
constexpr int kMaxToken = 64;

QString text(const QJsonValue &value, int limit)
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

qint64 boundedInt64(const QJsonValue &value, qint64 minimum, qint64 maximum, qint64 fallback)
{
    if (!value.isDouble())
        return fallback;
    const double number = value.toDouble();
    return number < double(minimum) || number > double(maximum) ? fallback : qint64(number);
}

QStringList stringList(const QJsonValue &value, int count, int limit)
{
    QStringList result;
    if (!value.isArray())
        return result;
    for (const QJsonValue &entry : value.toArray()) {
        if (result.size() == count)
            break;
        const QString string = text(entry, limit);
        if (!string.isEmpty())
            result.append(string);
    }
    return result;
}

QString firstTag(const QJsonValue &array)
{
    if (!array.isArray() || array.toArray().isEmpty())
        return {};
    return text(array.toArray().first(), kMaxToken);
}

QString validId(const QJsonValue &value)
{
    const QString id = text(value, kMaxToken);
    return isValidJellyfinId(id) ? id : QString();
}

// The day of an ISO 8601 timestamp: the server writes midnight UTC, and a time zone must not move the day
QDate dateOf(const QJsonValue &value)
{
    const QDate date = QDate::fromString(text(value, kMaxToken).left(10), Qt::ISODate);
    return date.isValid() && date.year() >= 1800 ? date : QDate();
}

// The names of a list of `NameGuidPair` entries; an entry with no name is dropped
QStringList pairNames(const QJsonValue &value, int count)
{
    QStringList result;
    if (!value.isArray())
        return result;
    for (const QJsonValue &entry : value.toArray()) {
        if (result.size() == count)
            break;
        if (!entry.isObject())
            continue;
        const QJsonObject pair = entry.toObject();
        const QString name = text(pair.value(QStringLiteral("Name")), 128);
        if (!name.isEmpty())
            result.append(name);
    }
    return result;
}

StreamSummary parseStream(const QJsonObject &stream)
{
    StreamSummary summary;
    summary.type = text(stream.value(QStringLiteral("Type")), kMaxToken);
    summary.codec = text(stream.value(QStringLiteral("Codec")), kMaxToken);
    summary.language = text(stream.value(QStringLiteral("Language")), kMaxToken);
    summary.title = text(stream.value(QStringLiteral("DisplayTitle")), ItemDetails::MaxShort);
    if (summary.title.isEmpty())
        summary.title = text(stream.value(QStringLiteral("Title")), ItemDetails::MaxShort);
    summary.width = boundedInt(stream.value(QStringLiteral("Width")), 0, 100000, 0);
    summary.height = boundedInt(stream.value(QStringLiteral("Height")), 0, 100000, 0);
    summary.channels = boundedInt(stream.value(QStringLiteral("Channels")), 0, 64, 0);
    summary.videoRangeType = text(stream.value(QStringLiteral("VideoRangeType")), kMaxToken);
    summary.channelLayout = text(stream.value(QStringLiteral("ChannelLayout")), kMaxToken);
    summary.bitrate = boundedInt64(stream.value(QStringLiteral("BitRate")), 0, 100'000'000'000, 0);
    summary.isDefault = stream.value(QStringLiteral("IsDefault")).toBool(false);
    summary.isForced = stream.value(QStringLiteral("IsForced")).toBool(false);
    summary.isExternal = stream.value(QStringLiteral("IsExternal")).toBool(false);
    return summary;
}

SourceSummary parseSource(const QJsonObject &source)
{
    SourceSummary summary;
    summary.id = text(source.value(QStringLiteral("Id")), kMaxToken);
    summary.name = text(source.value(QStringLiteral("Name")), ItemDetails::MaxShort);
    summary.container = text(source.value(QStringLiteral("Container")), kMaxToken);
    summary.size = boundedInt64(source.value(QStringLiteral("Size")), 0, 1'000'000'000'000'000, 0);
    summary.bitrate = boundedInt64(source.value(QStringLiteral("Bitrate")), 0, 100'000'000'000, 0);
    const QJsonValue streams = source.value(QStringLiteral("MediaStreams"));
    if (streams.isArray()) {
        for (const QJsonValue &stream : streams.toArray()) {
            if (summary.streams.size() == ItemDetails::MaxStreams)
                break;
            if (!stream.isObject())
                continue;
            StreamSummary parsed = parseStream(stream.toObject());
            // Attachments (fonts) and data streams are not shown and must not push real tracks past the cap
            if (parsed.type == QLatin1String("Video") || parsed.type == QLatin1String("Audio")
                || parsed.type == QLatin1String("Subtitle"))
                summary.streams.append(std::move(parsed));
        }
    }
    // The server's path to the file is never read: it says nothing the page may show
    return summary;
}
} // namespace

ItemDetails parseItemDetails(const QJsonObject &item)
{
    ItemDetails details;
    const QString id = validId(item.value(QStringLiteral("Id")));
    details.valid = !id.isEmpty();
    if (!details.valid)
        return details;
    details.id = id;
    details.name = text(item.value(QStringLiteral("Name")), ItemDetails::MaxName);
    details.type = text(item.value(QStringLiteral("Type")), kMaxToken);
    details.overview =
        overviewText(item.value(QStringLiteral("Overview")), ItemDetails::MaxOverview);
    details.tagline =
        stringList(item.value(QStringLiteral("Taglines")), 1, ItemDetails::MaxShort).value(0);
    details.year = boundedInt(item.value(QStringLiteral("ProductionYear")), 1, 9999, 0);
    const qint64 ticks = boundedInt64(item.value(QStringLiteral("RunTimeTicks")), 1, kMaxTicks, 0);
    details.runtimeMinutes = ticks > 0 ? int(std::max<qint64>(1, ticks / kTicksPerMinute)) : -1;
    const QJsonValue rating = item.value(QStringLiteral("CommunityRating"));
    if (rating.isDouble() && rating.toDouble() >= 0 && rating.toDouble() <= 10)
        details.communityRating = rating.toDouble();
    details.officialRating = text(item.value(QStringLiteral("OfficialRating")), kMaxToken);
    details.genres = stringList(item.value(QStringLiteral("Genres")), ItemDetails::MaxGenres, 128);
    details.studios = pairNames(item.value(QStringLiteral("Studios")), ItemDetails::MaxStudios);
    details.premiereDate = dateOf(item.value(QStringLiteral("PremiereDate")));
    details.endDate = dateOf(item.value(QStringLiteral("EndDate")));
    const QString status = text(item.value(QStringLiteral("Status")), kMaxToken);
    if (status == QLatin1String("Continuing") || status == QLatin1String("Ended")
        || status == QLatin1String("Unreleased"))
        details.seriesStatus = status;
    const QJsonValue critic = item.value(QStringLiteral("CriticRating"));
    if (critic.isDouble() && critic.toDouble() >= 0 && critic.toDouble() <= 100)
        details.criticRating = critic.toDouble();

    const QJsonValue data = item.value(QStringLiteral("UserData"));
    if (data.isObject()) {
        const QJsonObject userData = data.toObject();
        details.played = userData.value(QStringLiteral("Played")).toBool(false);
        details.favorite = userData.value(QStringLiteral("IsFavorite")).toBool(false);
        details.unplayedCount =
            boundedInt(userData.value(QStringLiteral("UnplayedItemCount")), 0, 1'000'000, -1);
        details.resumePositionTicks =
            boundedInt64(userData.value(QStringLiteral("PlaybackPositionTicks")), 0, kMaxTicks, 0);
        const QJsonValue percentage = userData.value(QStringLiteral("PlayedPercentage"));
        if (percentage.isDouble() && percentage.toDouble() >= 0 && percentage.toDouble() <= 100)
            details.playedPercentage = percentage.toDouble();
    }

    details.seriesName = text(item.value(QStringLiteral("SeriesName")), ItemDetails::MaxName);
    details.seriesId = validId(item.value(QStringLiteral("SeriesId")));
    details.seasonId = validId(item.value(QStringLiteral("SeasonId")));
    details.seasonNumber = boundedInt(item.value(QStringLiteral("ParentIndexNumber")), 0, 9999, -1);
    details.episodeNumber = boundedInt(item.value(QStringLiteral("IndexNumber")), 0, 99999, -1);
    // A season's own IndexNumber is its number: it is not an episode number
    details.episodeNumberEnd =
        boundedInt(item.value(QStringLiteral("IndexNumberEnd")), 0, 99999, -1);
    if (details.type == QLatin1String("Season")) {
        details.seasonNumber = details.episodeNumber;
        details.episodeNumber = -1;
        details.episodeNumberEnd = -1;
    }

    const QJsonValue tags = item.value(QStringLiteral("ImageTags"));
    const QJsonObject tagObject = tags.isObject() ? tags.toObject() : QJsonObject();
    details.primaryTag = text(tagObject.value(QStringLiteral("Primary")), kMaxToken);
    const QString ownBackdrop = firstTag(item.value(QStringLiteral("BackdropImageTags")));
    if (!ownBackdrop.isEmpty()) {
        details.backdropItemId = details.id;
        details.backdropTag = ownBackdrop;
    } else {
        const QString parent = validId(item.value(QStringLiteral("ParentBackdropItemId")));
        const QString parentTag = firstTag(item.value(QStringLiteral("ParentBackdropImageTags")));
        if (!parent.isEmpty() && !parentTag.isEmpty()) {
            details.backdropItemId = parent;
            details.backdropTag = parentTag;
        }
    }
    const QString ownLogo = text(tagObject.value(QStringLiteral("Logo")), kMaxToken);
    if (!ownLogo.isEmpty()) {
        details.logoItemId = details.id;
        details.logoTag = ownLogo;
    } else {
        const QString parent = validId(item.value(QStringLiteral("ParentLogoItemId")));
        const QString parentTag = text(item.value(QStringLiteral("ParentLogoImageTag")), kMaxToken);
        if (!parent.isEmpty() && !parentTag.isEmpty()) {
            details.logoItemId = parent;
            details.logoTag = parentTag;
        }
    }

    const QJsonValue people = item.value(QStringLiteral("People"));
    if (people.isArray()) {
        for (const QJsonValue &entry : people.toArray()) {
            if (details.people.size() == ItemDetails::MaxPeople)
                break;
            if (!entry.isObject())
                continue;
            const QJsonObject person = entry.toObject();
            PersonSummary summary;
            summary.name = text(person.value(QStringLiteral("Name")), ItemDetails::MaxShort);
            if (summary.name.isEmpty())
                continue;
            summary.id = validId(person.value(QStringLiteral("Id")));
            summary.role = text(person.value(QStringLiteral("Role")), ItemDetails::MaxShort);
            summary.type = text(person.value(QStringLiteral("Type")), kMaxToken);
            summary.primaryTag = text(person.value(QStringLiteral("PrimaryImageTag")), kMaxToken);
            if (summary.type == QLatin1String("Director"))
                details.directors.append(summary.name);
            else if (summary.type == QLatin1String("Writer"))
                details.writers.append(summary.name);
            details.people.append(summary);
        }
    }

    const QJsonValue chapters = item.value(QStringLiteral("Chapters"));
    if (chapters.isArray()) {
        for (const QJsonValue &entry : chapters.toArray()) {
            if (details.chapters.size() == ItemDetails::MaxChapters)
                break;
            if (!entry.isObject())
                continue;
            const QJsonObject chapter = entry.toObject();
            ChapterSummary summary;
            summary.name = text(chapter.value(QStringLiteral("Name")), ItemDetails::MaxShort);
            summary.startTicks =
                boundedInt64(chapter.value(QStringLiteral("StartPositionTicks")), 0, kMaxTicks, 0);
            details.chapters.append(summary);
        }
    }

    const QJsonValue sources = item.value(QStringLiteral("MediaSources"));
    if (sources.isArray()) {
        for (const QJsonValue &entry : sources.toArray()) {
            if (details.sources.size() == ItemDetails::MaxSources)
                break;
            if (entry.isObject())
                details.sources.append(parseSource(entry.toObject()));
        }
    }
    return details;
}
} // namespace Kinefin
