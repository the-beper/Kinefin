// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDate>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace Kinefin {
struct PersonSummary {
    QString id;
    QString name;
    QString role;
    QString type;
    QString primaryTag;
};

// A remote trailer: an https URL only, never with credentials, and its name
struct TrailerSummary {
    QString name;
    QString url;
};

struct ChapterSummary {
    QString name;
    qint64 startTicks = 0;
};

struct StreamSummary {
    // The server's stream index, -1 when it sends none
    int index = -1;
    QString type; // Video, Audio or Subtitle
    QString codec;
    QString language;
    QString title;
    int width = 0;
    int height = 0;
    int channels = 0;
    // The server's `VideoRangeType` and `ChannelLayout` tokens, empty when it sends none
    QString videoRangeType;
    QString channelLayout;
    qint64 bitrate = 0;
    // Video and audio technical facts, empty or zero when the server sends none; `level` is the server's number (41 for H.264 level 4.1)
    QString profile;
    QString pixelFormat;
    // `VideoRange` (SDR, HDR)
    QString videoRange;
    QString colorSpace;
    QString colorPrimaries;
    QString colorTransfer;
    qreal level = 0;
    qreal frameRate = 0;
    int bitDepth = 0;
    int sampleRate = 0;
    bool isDefault = false;
    bool isForced = false;
    bool isExternal = false;
};

struct SourceSummary {
    QString id;
    QString name;
    QString container;
    // The server's entity tag of the file, empty when it sent none
    QString etag;
    qint64 size = 0;
    qint64 bitrate = 0;
    QList<StreamSummary> streams;
};

// What the details page shows of one `BaseItemDto`. Everything comes from the server and is untrusted: strings and lists are capped, a value of the wrong type is ignored, and the server's file path is never read. `valid` is false for an entry without a valid `Id`
struct ItemDetails {
    static constexpr int MaxName = 512;
    static constexpr int MaxOverview = 16384;
    static constexpr int MaxShort = 256;
    static constexpr int MaxGenres = 20;
    static constexpr int MaxStudios = 8;
    static constexpr int MaxPeople = 100;
    static constexpr int MaxTrailers = 8;
    static constexpr int MaxChapters = 200;
    static constexpr int MaxSources = 8;
    static constexpr int MaxStreams = 64;

    bool valid = false;
    QString id;
    QString name;
    QString type;
    QString overview;
    QString tagline;
    int year = 0;
    int runtimeMinutes = -1;
    qreal communityRating = -1;
    QString officialRating;
    QStringList genres;
    // The studios, or the networks of a series
    QStringList studios;
    // Invalid when the server sends no date; the time of day is dropped
    QDate premiereDate;
    QDate endDate;
    // Continuing, Ended or Unreleased; empty for anything else
    QString seriesStatus;
    // Percent (0 to 100), -1 when unknown
    qreal criticRating = -1;
    QStringList directors;
    QStringList writers;
    bool played = false;
    bool favorite = false;
    // The item's CanDownload when the server sent one; absent or unreadable leaves it true, so the policy decides
    bool canDownload = true;
    // An episode the server only knows of (LocationType Virtual, no file): nothing to play or download. A season of a series without season folders is Virtual too, so only an episode counts
    bool virtualItem = false;
    // `CanDelete` from the server; false unless it says so, so no delete is ever sent blindly
    bool canDelete = false;
    qreal playedPercentage = 0;
    int unplayedCount = -1;
    qint64 resumePositionTicks = 0;
    QString seriesName;
    QString seriesId;
    QString seasonId;
    int seasonNumber = -1;
    int episodeNumber = -1;
    int episodeNumberEnd = -1;
    QString primaryTag;
    // The image of the item itself, or else the parent's, with the id that owns it
    QString backdropItemId;
    QString backdropTag;
    QString logoItemId;
    QString logoTag;
    QList<PersonSummary> people;
    QList<TrailerSummary> trailers;
    QList<ChapterSummary> chapters;
    QList<SourceSummary> sources;
};

ItemDetails parseItemDetails(const QJsonObject &item);

// The `Chapters` array of a `BaseItemDto` in the server's order, for the details page and the player: at most MaxChapters, names cut at MaxShort and made one line (no control, format or line-separator characters), an unreadable start read as 0
QList<ChapterSummary> parseChapters(const QJsonValue &chapters);

// One `MediaSourceInfo` with the same caps as the details page: also used for the source the player opened
SourceSummary parseMediaSource(const QJsonObject &source);
} // namespace Kinefin
