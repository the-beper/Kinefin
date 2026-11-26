// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "PersonItemsModel.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>

#include <algorithm>

namespace Kinefin {
namespace {
QString idKey(const QString &id)
{
    return QString(id).remove(QLatin1Char('-')).toLower();
}

// The card of a series the person only has episodes in: the series' own name and images, as the episode carries them
ItemSummary seriesCardOf(const ItemSummary &episode)
{
    ItemSummary card;
    card.valid = true;
    card.id = episode.seriesId;
    card.name = episode.seriesName;
    card.type = QStringLiteral("Series");
    card.thumbTag = episode.seriesThumbTag;
    card.primaryTag = episode.seriesPrimaryTag;
    return card;
}
} // namespace

PersonItemsModel::PersonItemsModel(JellyfinClient *client, QObject *parent)
    : QAbstractListModel(parent), m_client(client), m_seriesEpisodes(client, this)
{
    if (m_client) {
        connect(m_client, &JellyfinClient::itemsReady, this, &PersonItemsModel::onItemsReady);
        connect(m_client, &JellyfinClient::listingFailed, this, &PersonItemsModel::onListingFailed);
        // A manual watched-state edit changes the badges of the resident rows
        connect(m_client, &JellyfinClient::itemPlayedUpdated, this,
                [this](const QString &id, bool played) {
                    if (m_open)
                        update(id, [played](ItemSummary &item) { applyPlayed(item, played); });
                });
        connect(m_client, &JellyfinClient::itemProgressUpdated, this,
                [this](const QString &id, qint64 position, double percentage) {
                    if (m_open)
                        update(id, [position, percentage](ItemSummary &item) {
                            applyProgress(item, position, percentage);
                        });
                });
        connect(m_client, &JellyfinClient::itemUnplayedCountUpdated, this,
                [this](const QString &id, int count) {
                    if (m_open)
                        update(id, [count](ItemSummary &item) { item.unplayedCount = count; });
                });
        connect(m_client, &JellyfinClient::itemFavoriteUpdated, this,
                [this](const QString &id, bool favorite) {
                    if (m_open)
                        update(id, [favorite](ItemSummary &item) { item.favorite = favorite; });
                });
        // A 401, a disconnect, a server switch and a sign-out all end with this signal
        connect(m_client, &JellyfinClient::credentialsCleared, this, [this] { close(); });
    }
}

bool PersonItemsModel::open(const QString &personId)
{
    if (!m_client || personId.isEmpty())
        return false;
    close();
    m_personId = personId;
    m_open = true;
    setStatus(Loading);
    m_titlesPending = sendTitles();
    m_episodesPending = sendEpisodes();
    // A request is refused without being sent when the id is not one the server could have sent
    if (!m_titlesPending || !m_episodesPending) {
        close();
        return false;
    }
    return true;
}

void PersonItemsModel::close()
{
    m_open = false;
    m_personId.clear();
    m_titlesPending = m_episodesPending = false;
    m_titlesFailed = m_episodesFailed = false;
    m_titlesNext = m_episodesNext = 0;
    const bool hadRows = !m_rows.isEmpty();
    beginResetModel();
    m_titles.clear();
    m_episodes.clear();
    m_rows.clear();
    endResetModel();
    m_seriesEpisodes.clear();
    if (hadRows)
        Q_EMIT totalCountChanged();
    setPageError(false);
    setStatus(Idle);
    if (m_client) {
        m_client->releaseListingChannel(Channel);
        m_client->releaseListingChannel(EpisodesChannel);
    }
}

void PersonItemsModel::retry()
{
    if (!m_open)
        return;
    const bool wasFailed = m_status == Failed;
    if (m_titlesFailed) {
        m_titlesPending = sendTitles();
        m_titlesFailed = !m_titlesPending;
    }
    if (m_episodesFailed) {
        m_episodesPending = sendEpisodes();
        m_episodesFailed = !m_episodesPending;
    }
    if (!m_titlesPending && !m_episodesPending)
        return;
    setPageError(false);
    if (wasFailed)
        setStatus(Loading);
}

bool PersonItemsModel::showSeries(const QString &seriesId)
{
    QList<ItemSummary> episodes;
    for (const ItemSummary &episode : std::as_const(m_episodes)) {
        if (sameItemId(episode.seriesId, seriesId))
            episodes.append(episode);
    }
    if (episodes.isEmpty())
        return false;
    m_seriesEpisodes.setEpisodes(std::move(episodes));
    return true;
}

int PersonItemsModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QVariant PersonItemsModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size())
        return {};
    const Row &row = m_rows.at(index.row());
    const ItemSummary &item = row.item;
    switch (role) {
    case LoadedRole:
        return true;
    case Qt::DisplayRole:
    case NameRole:
        return item.name;
    case ItemIdRole:
        return item.id;
    case YearRole:
        return item.year;
    case TypeRole:
        return item.type;
    case ImageUrlRole:
        return m_client ? cardImageUrl(m_client, item, true, ImageWidth) : QUrl();
    case PlayedRole:
        return item.played;
    case UnplayedCountRole:
        return item.unplayedCount;
    case PlayedPercentageRole:
        return item.playedPercentage;
    case FavoriteRole:
        return item.favorite;
    case GuestEpisodesRole:
        return row.guestEpisodes;
    default:
        return cardRoleData(item, role);
    }
}

QHash<int, QByteArray> PersonItemsModel::roleNames() const
{
    QHash<int, QByteArray> names{{ItemIdRole, "itemId"},
                                 {NameRole, "name"},
                                 {YearRole, "year"},
                                 {TypeRole, "type"},
                                 {ImageUrlRole, "imageUrl"},
                                 {PlayedRole, "played"},
                                 {UnplayedCountRole, "unplayedCount"},
                                 {PlayedPercentageRole, "playedPercentage"},
                                 {FavoriteRole, "favorite"},
                                 {GuestEpisodesRole, "guestEpisodes"},
                                 {LoadedRole, "loaded"}};
    names.insert(cardRoleNames());
    return names;
}

bool PersonItemsModel::sendTitles()
{
    if (!m_client || m_personId.isEmpty())
        return false;
    ItemQuery query;
    query.personIds = {m_personId};
    query.recursive = true;
    query.includeItemTypes = QStringLiteral("Movie,Series");
    query.sortBy = QStringLiteral("SortName");
    query.cardImages = true;
    query.startIndex = m_titlesNext;
    query.limit = std::min(PageSize, Limit - m_titlesNext);
    return m_client->fetchItems(query, Channel);
}

bool PersonItemsModel::sendEpisodes()
{
    if (!m_client || m_personId.isEmpty())
        return false;
    ItemQuery query;
    query.personIds = {m_personId};
    query.recursive = true;
    query.includeItemTypes = QStringLiteral("Episode");
    // Grouped by series, so a person above the cap loses the last series of the order, not episodes scattered over all of them
    query.sortBy = QStringLiteral("SeriesSortName,ParentIndexNumber,IndexNumber");
    query.fields = {QStringLiteral("Overview")};
    query.cardImages = true;
    query.startIndex = m_episodesNext;
    query.limit = std::min(PageSize, Limit - m_episodesNext);
    return m_client->fetchItems(query, EpisodesChannel);
}

void PersonItemsModel::onItemsReady(const QJsonArray &items, int total, int channel)
{
    if (!m_open)
        return;
    const bool titles = channel == Channel && m_titlesPending;
    if (!titles && !(channel == EpisodesChannel && m_episodesPending))
        return;
    int &next = titles ? m_titlesNext : m_episodesNext;
    QList<ItemSummary> &kept = titles ? m_titles : m_episodes;
    // A server that sends more than the page asked for is read up to it
    const qsizetype count = std::min<qsizetype>(items.size(), std::min(PageSize, Limit - next));
    for (qsizetype at = 0; at < count; ++at) {
        const ItemSummary item = parseItemSummary(items.at(at).toObject());
        if (!item.valid)
            continue;
        const bool isTitle =
            item.type == QLatin1String("Movie") || item.type == QLatin1String("Series");
        // An episode without its series' id and name has no card to go under
        const bool isEpisode = item.type == QLatin1String("Episode") && !item.seriesId.isEmpty()
                               && !item.seriesName.isEmpty();
        if (titles ? isTitle : isEpisode)
            kept.append(item);
    }
    next += int(count);
    // The next page while the server has more and the cap is not reached; an empty page ends the list
    bool cutShort = false;
    if (count > 0 && next < std::min(total, Limit)) {
        if (titles ? sendTitles() : sendEpisodes())
            return;
        // A refused page leaves the list short: the warning and Retry say so
        cutShort = true;
    }
    (titles ? m_titlesPending : m_episodesPending) = false;
    (titles ? m_titlesFailed : m_episodesFailed) = cutShort;
    settle();
}

void PersonItemsModel::onListingFailed(const QString &family, int, const QString &, int channel)
{
    if (family != QLatin1String("Items") || !m_open)
        return;
    if (channel == Channel && m_titlesPending) {
        m_titlesPending = false;
        m_titlesFailed = true;
    } else if (channel == EpisodesChannel && m_episodesPending) {
        m_episodesPending = false;
        m_episodesFailed = true;
    } else {
        return;
    }
    settle();
}

void PersonItemsModel::settle()
{
    if (m_titlesPending || m_episodesPending)
        return;
    QList<Row> rows;
    QSet<QString> shownSeries;
    for (const ItemSummary &title : std::as_const(m_titles)) {
        rows.append({title, 0});
        if (title.type == QLatin1String("Series"))
            shownSeries.insert(idKey(title.id));
    }
    // A series the person is in the main cast of stays one card that opens the series; the others get a card with the count of their episodes, in name order among the titles
    QHash<QString, int> guestRow;
    QList<Row> guests;
    for (const ItemSummary &episode : std::as_const(m_episodes)) {
        const QString key = idKey(episode.seriesId);
        if (shownSeries.contains(key))
            continue;
        const auto found = guestRow.constFind(key);
        if (found == guestRow.constEnd()) {
            guestRow.insert(key, int(guests.size()));
            guests.append({seriesCardOf(episode), 1});
        } else {
            ++guests[*found].guestEpisodes;
        }
    }
    std::stable_sort(guests.begin(), guests.end(), [](const Row &a, const Row &b) {
        return QString::localeAwareCompare(a.item.name, b.item.name) < 0;
    });
    // One pass over both sorted lists: inserting into the titles would move them once per guest card
    QList<Row> merged;
    merged.reserve(rows.size() + guests.size());
    qsizetype at = 0;
    for (const Row &guest : std::as_const(guests)) {
        while (at < rows.size()
               && QString::localeAwareCompare(rows.at(at).item.name, guest.item.name) <= 0)
            merged.append(rows.at(at++));
        merged.append(guest);
    }
    while (at < rows.size())
        merged.append(rows.at(at++));
    const int before = totalCount();
    beginResetModel();
    m_rows = std::move(merged);
    endResetModel();
    if (totalCount() != before)
        Q_EMIT totalCountChanged();
    // With nothing to show, one failed request fails the page: an empty page would say the person is in nothing
    const bool anyFailed = m_titlesFailed || m_episodesFailed;
    const bool failed = (m_titlesFailed && m_episodesFailed) || (anyFailed && m_rows.isEmpty());
    setPageError(!failed && anyFailed);
    setStatus(failed ? Failed : Ready);
}

void PersonItemsModel::update(const QString &itemId,
                              const std::function<void(ItemSummary &)> &change)
{
    for (qsizetype row = 0; row < m_rows.size(); ++row) {
        if (!sameItemId(m_rows.at(row).item.id, itemId))
            continue;
        change(m_rows[row].item);
        Q_EMIT dataChanged(index(int(row)), index(int(row)));
    }
    for (QList<ItemSummary> *list : {&m_titles, &m_episodes}) {
        for (ItemSummary &item : *list) {
            if (sameItemId(item.id, itemId))
                change(item);
        }
    }
    m_seriesEpisodes.update(itemId, change);
}

void PersonItemsModel::setStatus(Status status)
{
    if (m_status == status)
        return;
    m_status = status;
    Q_EMIT statusChanged();
}

void PersonItemsModel::setPageError(bool error)
{
    if (m_pageError == error)
        return;
    m_pageError = error;
    Q_EMIT pageErrorChanged();
}
} // namespace Kinefin
