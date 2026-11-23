// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ItemChildrenModel.h"

#include "models/ItemSummary.h"

#include <QJsonArray>

#include <algorithm>
#include <limits>

namespace Kinefin {

ItemChildrenModel::ItemChildrenModel(JellyfinClient *client, int channel, QObject *parent)
    : QObject(parent), m_client(client), m_channel(channel), m_seasons(client, false, this),
      m_items(client, false, this), m_episodes(client, channel, this)
{
    if (m_client) {
        connect(m_client, &JellyfinClient::seasonsReady, this, &ItemChildrenModel::onSeasonsReady);
        connect(m_client, &JellyfinClient::nextUpReady, this, &ItemChildrenModel::onNextUpReady);
        connect(m_client, &JellyfinClient::itemsReady, this, &ItemChildrenModel::onItemsReady);
        connect(m_client, &JellyfinClient::episodesReady, this,
                &ItemChildrenModel::onEpisodesReady);
        connect(m_client, &JellyfinClient::listingFailed, this,
                &ItemChildrenModel::onListingFailed);
        // A 401, a disconnect, a server switch and a sign-out all end with this signal
        connect(m_client, &JellyfinClient::credentialsCleared, this, [this] { clear(); });
    }
}

void ItemChildrenModel::load(const ItemDetails &details)
{
    Kind kind = None;
    if (details.type == QLatin1String("Series"))
        kind = Series;
    else if (details.type == QLatin1String("Season"))
        kind = Season;
    else if (details.type == QLatin1String("BoxSet"))
        kind = BoxSet;
    // The lists of another kind of item must not outlive it
    if (details.id != m_itemId || kind != m_kind)
        clear();
    m_itemId = details.id;
    m_seriesId = details.seriesId;
    setKind(kind);
    switch (kind) {
    case Series:
        requestSeasons();
        requestNextUp();
        break;
    case Season:
        m_seasonFirstPending = true;
        requestEpisodes();
        // The list refuses a season with no valid series id without a word: no first page will come
        if (m_episodes.status() == PagedItemsModel::Failed)
            m_seasonFirstPending = false;
        requestSeasonTarget(true);
        break;
    case BoxSet:
        requestItems();
        break;
    case None:
        break;
    }
    updatePlayTargetsReady();
}

void ItemChildrenModel::clear()
{
    m_itemId.clear();
    m_seriesId.clear();
    m_nextUpLoading = false;
    m_seasonFirstPending = false;
    m_target = Target::None;
    setKind(None);
    m_seasons.setItems({});
    m_items.setItems({});
    m_episodes.clear();
    setNextUp({});
    setFirstEpisode({});
    if (m_playTargetsReady) {
        m_playTargetsReady = false;
        Q_EMIT playTargetsReadyChanged();
    }
    setSeasonsStatus(Idle);
    setItemsStatus(Idle);
    if (m_itemsTotal != 0) {
        m_itemsTotal = 0;
        Q_EMIT itemsTotalChanged();
    }
}

void ItemChildrenModel::retry()
{
    if (m_kind == Series && m_seasonsStatus == Failed)
        requestSeasons();
    if (m_kind == BoxSet && m_itemsStatus == Failed)
        requestItems();
}

void ItemChildrenModel::refreshSeasonTargets()
{
    if (m_kind == Season)
        requestSeasonTarget(false);
}

void ItemChildrenModel::requestSeasons()
{
    setSeasonsStatus(Loading);
    if (!m_client || !m_client->fetchSeasons(m_itemId, m_channel))
        setSeasonsStatus(Failed);
}

void ItemChildrenModel::requestNextUp()
{
    // The next episode is a courtesy: its failure leaves the entry out without a message
    m_nextUpLoading =
        m_client && m_client->fetchNextUp(m_itemId, QString(), QString(), false, 1, m_channel);
}

// The first of a few episodes the server lists that has a file (a missing one cannot play), in the shape of a row of the episode list
QVariantMap ItemChildrenModel::firstPlayableEpisode(const QJsonArray &items) const
{
    for (const QJsonValue &value : items) {
        const ItemSummary summary = parseItemSummary(value.toObject());
        if (summary.valid && !summary.virtualItem && summary.type == QLatin1String("Episode"))
            return SeasonEpisodesModel::rowMap(m_client, summary);
    }
    return {};
}

// The first season that is not the Specials, or the first one there is; its first episode is the one a series starts from
void ItemChildrenModel::requestFirstEpisodeOfSeries(const QJsonArray &seasons)
{
    QString seasonId;
    int seasonNumber = std::numeric_limits<int>::max();
    for (const QJsonValue &value : seasons) {
        const ItemSummary season = parseItemSummary(value.toObject());
        if (!season.valid)
            continue;
        const bool better =
            seasonId.isEmpty() || (season.seasonNumber >= 1 && season.seasonNumber < seasonNumber);
        if (better) {
            seasonId = season.id;
            seasonNumber =
                season.seasonNumber >= 1 ? season.seasonNumber : std::numeric_limits<int>::max();
        }
    }
    m_target = Target::None;
    if (!seasonId.isEmpty() && m_client
        && m_client->fetchEpisodes(m_itemId, seasonId, 0, TargetEpisodes, m_channel))
        m_target = Target::SeriesFirst;
    else
        setFirstEpisode({});
}

// A season's first episode, or its first unplayed one: the server sorts a season's episodes by number; a few are read because a missing episode (no file) cannot play and is skipped
void ItemChildrenModel::requestSeasonTarget(bool unplayed)
{
    ItemQuery query;
    query.parentId = m_itemId;
    query.includeItemTypes = QStringLiteral("Episode");
    query.sortBy = QStringLiteral("IndexNumber");
    query.sortOrder = QStringLiteral("Ascending");
    query.limit = TargetEpisodes;
    if (unplayed)
        query.filters = QStringLiteral("IsUnplayed");
    // Both are a courtesy, like the Next Up entry: a failure leaves the button out without a message
    if (m_client && m_client->fetchItems(query, m_channel)) {
        m_target = unplayed ? Target::SeasonUnplayed : Target::SeasonFirst;
        return;
    }
    m_target = Target::None;
    if (unplayed)
        setNextUp({});
    else
        setFirstEpisode({});
}

void ItemChildrenModel::requestItems()
{
    ItemQuery query;
    query.parentId = m_itemId;
    query.limit = MaxCollectionItems;
    setItemsStatus(Loading);
    if (!m_client || !m_client->fetchItems(query, m_channel))
        setItemsStatus(Failed);
}

void ItemChildrenModel::requestEpisodes()
{
    m_episodes.open(m_seriesId, m_itemId);
}

void ItemChildrenModel::onSeasonsReady(const QJsonArray &items, int, int channel)
{
    if (channel != m_channel || m_kind != Series || m_seasonsStatus != Loading)
        return;
    m_seasons.setItems(items);
    setSeasonsStatus(Ready);
    requestFirstEpisodeOfSeries(items);
    updatePlayTargetsReady();
}

void ItemChildrenModel::onEpisodesReady(const QJsonArray &items, int, int channel)
{
    if (channel != m_channel)
        return;
    // The list's own first page answers for the season's first episode: it is the page the list asked for, and a later page is not looked at
    if (m_kind == Season && m_seasonFirstPending) {
        m_seasonFirstPending = false;
        setFirstEpisode(firstPlayableEpisode(items));
        updatePlayTargetsReady();
        return;
    }
    if (m_kind != Series || m_target != Target::SeriesFirst)
        return;
    m_target = Target::None;
    setFirstEpisode(firstPlayableEpisode(items));
    updatePlayTargetsReady();
}

void ItemChildrenModel::onNextUpReady(const QJsonArray &items, int, int channel)
{
    if (channel != m_channel || m_kind != Series || !m_nextUpLoading)
        return;
    m_nextUpLoading = false;
    const ItemSummary summary =
        items.isEmpty() ? ItemSummary() : parseItemSummary(items.first().toObject());
    setNextUp(summary.valid ? SeasonEpisodesModel::rowMap(m_client, summary) : QVariantMap());
    updatePlayTargetsReady();
}

void ItemChildrenModel::onItemsReady(const QJsonArray &items, int total, int channel)
{
    if (channel == m_channel && m_kind == Season
        && (m_target == Target::SeasonFirst || m_target == Target::SeasonUnplayed)) {
        const bool unplayed = m_target == Target::SeasonUnplayed;
        const QVariantMap found = firstPlayableEpisode(items);
        m_target = Target::None;
        if (unplayed) {
            setNextUp(found);
        } else {
            setFirstEpisode(found);
            requestSeasonTarget(true);
        }
        updatePlayTargetsReady();
        return;
    }
    if (channel != m_channel || m_kind != BoxSet || m_itemsStatus != Loading)
        return;
    m_items.setItems(items);
    const int shown = std::max(m_items.count(), std::min(total, PagedItemsModel::MaxRows));
    if (shown != m_itemsTotal) {
        m_itemsTotal = shown;
        Q_EMIT itemsTotalChanged();
    }
    setItemsStatus(Ready);
}

void ItemChildrenModel::onListingFailed(const QString &family, int, const QString &, int channel)
{
    if (channel != m_channel)
        return;
    if (family == QLatin1String("Seasons") && m_kind == Series && m_seasonsStatus == Loading)
        setSeasonsStatus(Failed);
    else if (family == QLatin1String("Episodes") && m_kind == Season && m_seasonFirstPending) {
        // The list shows its own failure; the first episode is a courtesy
        m_seasonFirstPending = false;
        setFirstEpisode({});
    } else if ((family == QLatin1String("Episodes") && m_target == Target::SeriesFirst)
               || (family == QLatin1String("Items")
                   && (m_target == Target::SeasonFirst || m_target == Target::SeasonUnplayed))) {
        // The first episode and the one to go on with are courtesies: their failure leaves the buttons out
        const Target failed = m_target;
        m_target = Target::None;
        if (failed == Target::SeasonUnplayed) {
            setNextUp({});
        } else {
            setFirstEpisode({});
            if (failed == Target::SeasonFirst)
                requestSeasonTarget(true);
        }
    } else if (family == QLatin1String("NextUp")) {
        // A failed refresh must not leave a stale entry behind
        m_nextUpLoading = false;
        setNextUp({});
    } else if (family == QLatin1String("Items") && m_kind == BoxSet && m_itemsStatus == Loading)
        setItemsStatus(Failed);
    updatePlayTargetsReady();
}

void ItemChildrenModel::setKind(Kind kind)
{
    if (kind == m_kind)
        return;
    m_kind = kind;
    Q_EMIT kindChanged();
}

void ItemChildrenModel::setSeasonsStatus(Status status)
{
    if (status == m_seasonsStatus)
        return;
    m_seasonsStatus = status;
    Q_EMIT seasonsStatusChanged();
}

void ItemChildrenModel::setItemsStatus(Status status)
{
    if (status == m_itemsStatus)
        return;
    m_itemsStatus = status;
    Q_EMIT itemsStatusChanged();
}

// Once the answers of the item are in, kept while the same item reloads (the old entries stay until the new ones replace them) and dropped by clear()
void ItemChildrenModel::updatePlayTargetsReady()
{
    if (m_playTargetsReady || m_target != Target::None || m_seasonFirstPending)
        return;
    const bool settled = m_kind == Season
                         || (m_kind == Series && m_seasonsStatus != Idle
                             && m_seasonsStatus != Loading && !m_nextUpLoading);
    if (!settled)
        return;
    m_playTargetsReady = true;
    Q_EMIT playTargetsReadyChanged();
}

void ItemChildrenModel::setFirstEpisode(const QVariantMap &firstEpisode)
{
    if (firstEpisode == m_firstEpisode)
        return;
    m_firstEpisode = firstEpisode;
    Q_EMIT firstEpisodeChanged();
}

void ItemChildrenModel::setNextUp(const QVariantMap &nextUp)
{
    if (nextUp == m_nextUp)
        return;
    m_nextUp = nextUp;
    Q_EMIT nextUpChanged();
}
} // namespace Kinefin
