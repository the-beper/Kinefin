// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ItemChildrenModel.h"

#include "models/ItemSummary.h"

#include <QJsonArray>

#include <algorithm>

namespace Kinefin {

ItemChildrenModel::ItemChildrenModel(JellyfinClient *client, int channel, QObject *parent)
    : QObject(parent), m_client(client), m_channel(channel), m_seasons(client, false, this),
      m_items(client, false, this), m_episodes(client, channel, this)
{
    if (m_client) {
        connect(m_client, &JellyfinClient::seasonsReady, this, &ItemChildrenModel::onSeasonsReady);
        connect(m_client, &JellyfinClient::nextUpReady, this, &ItemChildrenModel::onNextUpReady);
        connect(m_client, &JellyfinClient::itemsReady, this, &ItemChildrenModel::onItemsReady);
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
        requestEpisodes();
        break;
    case BoxSet:
        requestItems();
        break;
    case None:
        break;
    }
}

void ItemChildrenModel::clear()
{
    m_itemId.clear();
    m_seriesId.clear();
    m_nextUpLoading = false;
    setKind(None);
    m_seasons.setItems({});
    m_items.setItems({});
    m_episodes.clear();
    setNextUp({});
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
}

void ItemChildrenModel::onNextUpReady(const QJsonArray &items, int, int channel)
{
    if (channel != m_channel || m_kind != Series || !m_nextUpLoading)
        return;
    m_nextUpLoading = false;
    const ItemSummary summary =
        items.isEmpty() ? ItemSummary() : parseItemSummary(items.first().toObject());
    setNextUp(summary.valid ? SeasonEpisodesModel::rowMap(m_client, summary) : QVariantMap());
}

void ItemChildrenModel::onItemsReady(const QJsonArray &items, int total, int channel)
{
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
    else if (family == QLatin1String("NextUp")) {
        // A failed refresh must not leave a stale entry behind
        m_nextUpLoading = false;
        setNextUp({});
    } else if (family == QLatin1String("Items") && m_kind == BoxSet && m_itemsStatus == Loading)
        setItemsStatus(Failed);
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

void ItemChildrenModel::setNextUp(const QVariantMap &nextUp)
{
    if (nextUp == m_nextUp)
        return;
    m_nextUp = nextUp;
    Q_EMIT nextUpChanged();
}
} // namespace Kinefin
