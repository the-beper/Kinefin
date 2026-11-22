// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

// The card of a favorite, in the rows of the Favorites page and in the grid of a Show All page: an episode or a season shows its label under the series name
MediaCard {
    required property int seasonNumber
    required property int episodeNumber
    required property string episodeTitle

    // The row or the grid is the one tab stop, the arrow keys move inside it
    focusPolicy: Qt.NoFocus
    loaded: true
    subtitle: {
        if (episodeNumber >= 0) {
            const label = EpisodeLabel.label(seasonNumber, episodeNumber);
            return episodeTitle.length > 0 ? i18nc("episode label and episode title", "%1 — %2", label, episodeTitle) : label;
        }
        if (type === "Season" && episodeTitle.length > 0)
            return episodeTitle;
        return year > 0 ? String(year) : "";
    }
    spokenSubtitle: {
        if (episodeNumber >= 0) {
            const label = EpisodeLabel.spoken(seasonNumber, episodeNumber);
            return episodeTitle.length > 0 ? i18nc("episode label and episode title", "%1 — %2", label, episodeTitle) : label;
        }
        if (type === "Season" && episodeTitle.length > 0)
            return episodeTitle;
        return year > 0 ? String(year) : "";
    }
}
