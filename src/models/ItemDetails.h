// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

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

struct ChapterSummary {
    QString name;
    qint64 startTicks = 0;
};

struct StreamSummary {
    QString type; // Video, Audio or Subtitle
    QString codec;
    QString language;
    QString title;
    int width = 0;
    int height = 0;
    int channels = 0;
    qint64 bitrate = 0;
    bool isDefault = false;
    bool isForced = false;
    bool isExternal = false;
};

struct SourceSummary {
    QString id;
    QString name;
    QString container;
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
    static constexpr int MaxPeople = 100;
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
    QStringList directors;
    QStringList writers;
    bool played = false;
    bool favorite = false;
    qreal playedPercentage = 0;
    int unplayedCount = -1;
    qint64 resumePositionTicks = 0;
    QString seriesName;
    QString seriesId;
    int seasonNumber = -1;
    int episodeNumber = -1;
    QString primaryTag;
    // The image of the item itself, or else the parent's, with the id that owns it
    QString backdropItemId;
    QString backdropTag;
    QString logoItemId;
    QString logoTag;
    QList<PersonSummary> people;
    QList<ChapterSummary> chapters;
    QList<SourceSummary> sources;
};

ItemDetails parseItemDetails(const QJsonObject &item);
} // namespace Kinefin
