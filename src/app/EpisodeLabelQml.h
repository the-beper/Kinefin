// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>

namespace Kinefin {
// The season and episode label every episode card, row and page shows, built in one place so the wording and the catalog entries stay single. A negative season reads as unknown: only the episode number is shown
class EpisodeLabelQml : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(EpisodeLabel)
    QML_SINGLETON

  public:
    explicit EpisodeLabelQml(QObject *parent = nullptr) : QObject(parent) {}

    // "S1:E5", or "E5" without a season
    Q_INVOKABLE QString label(int seasonNumber, int episodeNumber) const;
    // The same for an episode file that covers several: "S1:E5–E6", "E5–E6"; a last number that is not past the first reads as one episode
    Q_INVOKABLE QString label(int seasonNumber, int episodeNumber, int episodeNumberEnd) const;
    // What a screen reader says in place of the label ("S1:E5" would be spelled out): "Season 1, Episode 5", "Episode 5"
    Q_INVOKABLE QString spoken(int seasonNumber, int episodeNumber) const;
};
} // namespace Kinefin
