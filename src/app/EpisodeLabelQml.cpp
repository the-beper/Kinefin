// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "EpisodeLabelQml.h"

#include <KLocalizedString>

namespace Kinefin {
QString EpisodeLabelQml::label(int seasonNumber, int episodeNumber) const
{
    if (seasonNumber >= 0)
        return i18nc("season and episode number", "S%1:E%2", seasonNumber, episodeNumber);
    return i18nc("episode number", "E%1", episodeNumber);
}

QString EpisodeLabelQml::label(int seasonNumber, int episodeNumber, int episodeNumberEnd) const
{
    if (episodeNumberEnd <= episodeNumber)
        return label(seasonNumber, episodeNumber);
    if (seasonNumber >= 0)
        return i18nc("season and episode range", "S%1:E%2–E%3", seasonNumber, episodeNumber,
                     episodeNumberEnd);
    return i18nc("episode number range", "E%1–E%2", episodeNumber, episodeNumberEnd);
}

QString EpisodeLabelQml::spoken(int seasonNumber, int episodeNumber) const
{
    if (seasonNumber >= 0)
        return i18nc("season and episode number spoken", "Season %1, Episode %2", seasonNumber,
                     episodeNumber);
    return i18nc("episode number spoken", "Episode %1", episodeNumber);
}
} // namespace Kinefin
