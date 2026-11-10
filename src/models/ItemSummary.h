// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonObject>
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
    QString seriesName;
    QString seriesId;
    QString seriesThumbTag;
    QString parentBackdropItemId;
    QString parentBackdropTag;
    int year = 0;
    int seasonNumber = -1;
    int episodeNumber = -1;
    int unplayedCount = -1;
    qreal playedPercentage = 0;
    bool played = false;
    bool favorite = false;
    bool valid = false;
};

ItemSummary parseItemSummary(const QJsonObject &item);
} // namespace Kinefin
