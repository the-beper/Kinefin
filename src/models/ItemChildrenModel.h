// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/JellyfinClient.h"
#include "models/ItemDetails.h"
#include "models/ItemRowModel.h"
#include "models/SeasonEpisodesModel.h"

#include <QObject>
#include <QPointer>
#include <QVariantMap>

namespace Kinefin {
// What hangs below an item on its details page: the seasons of a series and its Next Up episode, the episodes of a season and the episode to go on with (a season's first unplayed one), the first episode of both (it starts them from the beginning), or the items of a collection. It loads after the details are known (`load()`), on the one listing channel it is given, and keeps the old lists on screen until the answers replace them (a season's episodes restart from the top). Nothing retries by itself: `retry()` asks again for the seasons or the collection items that failed (the episodes have their own `retry()`, the Next Up entry is not retried). The model empties itself on `clear()` and when the client loses its credentials. Everything shown is server data: render it as plain text
class ItemChildrenModel final : public QObject {
    Q_OBJECT
    Q_PROPERTY(Kind kind READ kind NOTIFY kindChanged)
    // A series: its seasons (a poster row, with unplayed counts) and its Next Up episode
    Q_PROPERTY(Kinefin::ItemRowModel *seasons READ seasons CONSTANT)
    Q_PROPERTY(Status seasonsStatus READ seasonsStatus NOTIFY seasonsStatusChanged)
    Q_PROPERTY(bool hasNextUp READ hasNextUp NOTIFY nextUpChanged)
    // The episode to go on with: a series' Next Up (the server's answer, its first episode while nothing was watched), a season's first unplayed episode. Empty when there is none; otherwise the role names of SeasonEpisodesModel (itemId, name, seasonNumber, episodeNumber, positionTicks, ...)
    Q_PROPERTY(QVariantMap nextUp READ nextUp NOTIFY nextUpChanged)
    // The first episode of a series (of its first season after the Specials) or of a season, in the same shape; empty until the answer comes or when there is none
    Q_PROPERTY(bool hasFirstEpisode READ hasFirstEpisode NOTIFY firstEpisodeChanged)
    Q_PROPERTY(QVariantMap firstEpisode READ firstEpisode NOTIFY firstEpisodeChanged)
    // A series or a season has asked for the episode to go on with and for its first one and has had every answer or failure: the two are final, so a button built on them does not change under the user's pointer
    Q_PROPERTY(bool playTargetsReady READ playTargetsReady NOTIFY playTargetsReadyChanged)
    // A collection: its items (a poster row, the first `MaxCollectionItems`) and how many the server has
    Q_PROPERTY(Kinefin::ItemRowModel *items READ items CONSTANT)
    Q_PROPERTY(Status itemsStatus READ itemsStatus NOTIFY itemsStatusChanged)
    Q_PROPERTY(int itemsTotal READ itemsTotal NOTIFY itemsTotalChanged)
    // A season: its episodes
    Q_PROPERTY(Kinefin::SeasonEpisodesModel *episodes READ episodes CONSTANT)

  public:
    enum Kind { None, Series, Season, BoxSet };
    Q_ENUM(Kind)
    enum Status { Idle, Loading, Ready, Failed };
    Q_ENUM(Status)

    static constexpr int MaxCollectionItems = 100;
    // How many episodes are read to find the first that has a file
    static constexpr int TargetEpisodes = 10;

    ItemChildrenModel(JellyfinClient *client, int channel, QObject *parent = nullptr);

    // Starts loading what belongs below this item; an item of any other kind has no children
    void load(const ItemDetails &details);
    void clear();
    Q_INVOKABLE void retry();
    // Asks again for the episode to go on with and the first episode of a season, leaving its episode list as it is (the rows follow the changes of user data themselves)
    void refreshSeasonTargets();

    Kind kind() const { return m_kind; }
    ItemRowModel *seasons() { return &m_seasons; }
    Status seasonsStatus() const { return m_seasonsStatus; }
    bool hasNextUp() const { return !m_nextUp.isEmpty(); }
    QVariantMap nextUp() const { return m_nextUp; }
    bool hasFirstEpisode() const { return !m_firstEpisode.isEmpty(); }
    QVariantMap firstEpisode() const { return m_firstEpisode; }
    bool playTargetsReady() const { return m_playTargetsReady; }
    ItemRowModel *items() { return &m_items; }
    Status itemsStatus() const { return m_itemsStatus; }
    int itemsTotal() const { return m_itemsTotal; }
    SeasonEpisodesModel *episodes() { return &m_episodes; }

  Q_SIGNALS:
    void kindChanged();
    void seasonsStatusChanged();
    void nextUpChanged();
    void firstEpisodeChanged();
    void playTargetsReadyChanged();
    void itemsStatusChanged();
    void itemsTotalChanged();

  private:
    void requestSeasons();
    void requestNextUp();
    void requestItems();
    void requestEpisodes();
    QVariantMap firstPlayableEpisode(const QJsonArray &items) const;
    void requestFirstEpisodeOfSeries(const QJsonArray &seasons);
    void requestSeasonTarget(bool unplayed);
    void onSeasonsReady(const QJsonArray &items, int total, int channel);
    void onNextUpReady(const QJsonArray &items, int total, int channel);
    void onItemsReady(const QJsonArray &items, int total, int channel);
    void onEpisodesReady(const QJsonArray &items, int total, int channel);
    void onListingFailed(const QString &family, int status, const QString &reason, int channel);
    void setKind(Kind kind);
    void setSeasonsStatus(Status status);
    void setItemsStatus(Status status);
    void setNextUp(const QVariantMap &nextUp);
    void setFirstEpisode(const QVariantMap &firstEpisode);
    void updatePlayTargetsReady();

    QPointer<JellyfinClient> m_client;
    int m_channel;
    Kind m_kind = None;
    QString m_itemId;
    QString m_seriesId;
    Status m_seasonsStatus = Idle;
    Status m_itemsStatus = Idle;
    bool m_nextUpLoading = false;
    QVariantMap m_nextUp;
    // A series' first episode is asked of its first season once the seasons are known; a season asks for its first episode and then for its first unplayed one, both as a listing of its episodes on the Items family
    enum class Target { None, SeriesFirst, SeasonFirst, SeasonUnplayed };
    Target m_target = Target::None;
    QVariantMap m_firstEpisode;
    bool m_playTargetsReady = false;
    int m_itemsTotal = 0;
    ItemRowModel m_seasons;
    ItemRowModel m_items;
    SeasonEpisodesModel m_episodes;
};
} // namespace Kinefin
