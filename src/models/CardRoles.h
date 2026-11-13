// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "models/ItemSummary.h"

#include <QUrl>

#include <QByteArray>
#include <QHash>
#include <QVariant>

namespace Kinefin {
class JellyfinClient;

// The roles of a model that feeds `MediaCard` beyond its own: they sit far above the models' own `Qt::UserRole + n` roles. The series roles are -1 when the item has no such value
enum CardRole {
    SeriesIdRole = Qt::UserRole + 64,
    RatingRole,
    RemainingMinutesRole,
    IsNewRole,
    SeasonCountRole,
    EpisodeCountRole,
    // The share of a series' episodes watched, 0 to 1
    SeriesProgressRole,
    // The saved playback position in ticks, 0 when there is none
    ResumeTicksRole
};

QHash<int, QByteArray> cardRoleNames();

// The value of one card role of an item, or an invalid variant for another role. Season and episode counts and the series progress are for a series only
QVariant cardRoleData(const ItemSummary &item, int role);

// The image of an item's card, `width` pixels wide, or an empty URL when it has none or the client is missing. A wide card follows jellyfin-web's default: an episode shows its series' thumb, then its parent's backdrop, and only then its own image; a movie or series shows its own thumb, then its backdrop. A season without a poster of its own shows its series' poster
QUrl cardImageUrl(JellyfinClient *client, const ItemSummary &item, bool landscape, int width);
} // namespace Kinefin
