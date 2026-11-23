// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

namespace Kinefin {
// What a grid tile needs of one `BaseItemDto`. Everything comes from the server and is untrusted: strings are capped, a value of the wrong type is ignored, and an entry without a valid `Id` stays in the list as `valid = false` so row offsets keep matching `startIndex + i`
struct ItemSummary {
    QString id;
    QString name;
    QString type;
    QString primaryTag;
    QString thumbTag;
    QString backdropTag;
    QString bannerTag;
    QString discTag;
    QString logoTag;
    QString seriesName;
    QString seriesPrimaryTag;
    QString overview;
    QString seriesId;
    QString seriesThumbTag;
    QString parentBackdropItemId;
    QString parentBackdropTag;
    QString parentThumbItemId;
    QString parentThumbTag;
    int year = 0;
    int seasonNumber = -1;
    int episodeNumber = -1;
    int episodeNumberEnd = -1;
    int runtimeMinutes = -1;
    // `RunTimeTicks` as sent, 0 when unknown: the time left is counted from it, not from the rounded minutes
    qint64 runtimeTicks = 0;
    int unplayedCount = -1;
    int remainingMinutes = -1;
    // The saved playback position (`UserData.PlaybackPositionTicks`), 0 when there is none
    qint64 positionTicks = 0;
    // Seasons (`ChildCount`) and episodes (`RecursiveItemCount`) of a series
    int childCount = -1;
    int recursiveItemCount = -1;
    qreal playedPercentage = 0;
    qreal communityRating = -1;
    QDateTime dateCreated;
    // `PremiereDate`, invalid when the server sends none: a virtual episode that premieres after now is unaired, otherwise it is missing
    QDateTime premiereDate;
    bool played = false;
    bool favorite = false;
    // An episode the library lists but has no file for (LocationType Virtual)
    bool virtualItem = false;
    bool valid = false;
};

ItemSummary parseItemSummary(const QJsonObject &item);

// A watched-state write the server confirmed: it also clears the saved position, so the progress and the time left go with it
void applyPlayed(ItemSummary &item, bool played);
// Watched, as the lists show it: a rewatch stopped halfway keeps Played on the server but is in progress
bool watchedToEnd(const ItemSummary &item);
// A saved position the server confirmed (an Undo of Mark as Watched): the progress and the time left come back
void applyProgress(ItemSummary &item, qint64 positionTicks, double playedPercentage);

// A virtual item (the server only knows of it) that premieres after `nowUtc`: unaired, as opposed to missing
bool isUnaired(const ItemSummary &item, const QDateTime &nowUtc);

// How long an unplayed item counts as new
constexpr int NewItemDays = 7;

// Whether the New badge applies: unplayed and added within `NewItemDays` of `nowUtc` (a date in the future counts as new)
bool isRecentlyAdded(const ItemSummary &item, const QDateTime &nowUtc);

// Whether two item ids are the same one: a GUID is written dashed or dashless, in either case
bool sameItemId(const QString &a, const QString &b);

// The item types the player can open directly: the one rule for the details page and the play queue
bool isPlayableVideo(const QString &type);

// The text of an `Overview` field: capped, with the line breaks the server writes as <br> turned into newlines and any other markup left as literal text (the one rule for every overview the app shows)
QString overviewText(const QJsonValue &value, int limit);
} // namespace Kinefin
