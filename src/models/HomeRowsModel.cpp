// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "HomeRowsModel.h"

#include "models/LibraryItemsModel.h"

#include <QDate>
#include <QMap>

#include <algorithm>
#include <utility>

namespace Kinefin {
namespace {
bool hasRecentlyAdded(const QString &collectionType)
{
    return collectionType == QLatin1String("movies") || collectionType == QLatin1String("tvshows");
}
} // namespace

HomeRowsModel::HomeRowsModel(JellyfinClient *client, QObject *parent)
    : QAbstractListModel(parent), m_client(client)
{
    if (!m_client)
        return;
    connect(m_client, &JellyfinClient::resumeItemsReady, this,
            [this](const QJsonArray &items, int, int channel) {
                deliver(ContinueWatching, channel, items);
            });
    connect(m_client, &JellyfinClient::nextUpReady, this,
            [this](const QJsonArray &items, int, int channel) { deliver(NextUp, channel, items); });
    connect(
        m_client, &JellyfinClient::itemsReady, this,
        [this](const QJsonArray &items, int, int channel) { deliver(Favorites, channel, items); });
    connect(
        m_client, &JellyfinClient::latestItemsReady, this,
        [this](const QJsonArray &items, int channel) { deliver(RecentlyAdded, channel, items); });
    connect(m_client, &JellyfinClient::listingFailed, this,
            [this](const QString &family, int, const QString &, int channel) {
                fail(family, channel);
            });
    // Continue Watching, Next Up and Recently Added depend on the played flag, and Continue Watching on the position too; a refresh of user data reports many items in one turn, which make one reload
    m_reloadSoon.setSingleShot(true);
    m_reloadSoon.setInterval(250);
    connect(&m_reloadSoon, &QTimer::timeout, this, [this] {
        if (!m_rows.empty())
            reload();
    });
    connect(m_client, &JellyfinClient::itemPlayedUpdated, this, [this] { m_reloadSoon.start(); });
    connect(m_client, &JellyfinClient::itemProgressUpdated, this, [this] { m_reloadSoon.start(); });
    // A 401, a disconnect, a server switch and a sign-out all end with this signal
    connect(m_client, &JellyfinClient::credentialsCleared, this, [this] { clear(); });
}

void HomeRowsModel::load(const QVariantList &libraries, bool unplayedOnly)
{
    m_libraries = libraries;
    m_unplayedOnly = unplayedOnly;
    reload();
}

void HomeRowsModel::reload()
{
    std::vector<Row> rows;
    rows.push_back({ContinueWatching, {}, {}, Idle, FixedChannel, nullptr, false});
    rows.push_back({NextUp, {}, {}, Idle, FixedChannel, nullptr, false});
    if (m_favorites)
        rows.push_back({Favorites, {}, {}, Idle, FavoritesChannel, nullptr, false});
    else if (m_client)
        m_client->releaseListingChannel(FavoritesChannel);
    int index = 0;
    for (const QVariant &entry : std::as_const(m_libraries)) {
        const QVariantMap library = entry.toMap();
        const QString id = library.value(QStringLiteral("id")).toString();
        if (!hasRecentlyAdded(library.value(QStringLiteral("collectionType")).toString()))
            continue;
        rows.push_back({RecentlyAdded, id, library.value(QStringLiteral("name")).toString(), Idle,
                        FirstLatestChannel + index++, nullptr, false});
    }
    // The same rows are asked again in place: each keeps what it shows until its reply replaces it, so a refresh after a playback neither rebuilds the page nor shows the loading state
    if (sameLayout(rows)) {
        for (int row = 0; row < int(m_rows.size()); ++row)
            request(row);
        return;
    }
    for (Row &row : rows)
        row.items = itemsFor(row.kind);
    // One reset swaps the rows; the old models go after it. A late reply of the previous load finds its channel re-requested (the client aborts the old request when a new one takes the channel) or no row at all
    beginResetModel();
    std::vector<Row> old = std::move(m_rows);
    m_rows = std::move(rows);
    endResetModel();
    for (const Row &row : old)
        row.items->deleteLater();
    for (int row = 0; row < int(m_rows.size()); ++row)
        request(row);
}

bool HomeRowsModel::sameLayout(const std::vector<Row> &rows) const
{
    return std::equal(rows.begin(), rows.end(), m_rows.begin(), m_rows.end(),
                      [](const Row &a, const Row &b) {
                          return a.kind == b.kind && a.channel == b.channel
                                 && a.libraryId == b.libraryId && a.libraryName == b.libraryName;
                      });
}

ItemRowModel *HomeRowsModel::itemsFor(Kind kind)
{
    switch (kind) {
    case ContinueWatching:
    case NextUp:
        return new ItemRowModel(m_client, true, this);
    case Favorites:
        return new ItemRowModel(m_client, false, this);
    case RecentlyAdded:
        break;
    }
    return new ItemRowModel(m_client, false, this, false);
}

HomeRowsModel::Row HomeRowsModel::favoritesRow()
{
    return {Favorites, {}, {}, Idle, FavoritesChannel, itemsFor(Favorites), false};
}

void HomeRowsModel::setFavoritesEnabled(bool enabled)
{
    if (enabled == m_favorites)
        return;
    m_favorites = enabled;
    // Without rows there is nothing to change: the next load() reads the flag
    if (m_rows.empty())
        return;
    // Continue Watching and Next Up always come first
    constexpr int position = 2;
    if (enabled) {
        beginInsertRows({}, position, position);
        m_rows.insert(m_rows.begin() + position, favoritesRow());
        endInsertRows();
        request(position);
        return;
    }
    const int row = rowFor(Favorites, FavoritesChannel);
    if (row < 0)
        return;
    beginRemoveRows({}, row, row);
    ItemRowModel *items = m_rows[size_t(row)].items;
    m_rows.erase(m_rows.begin() + row);
    endRemoveRows();
    // The delegate may still be bound to the list for a turn
    items->deleteLater();
    if (m_client)
        m_client->releaseListingChannel(FavoritesChannel);
}

void HomeRowsModel::retry(int row)
{
    if (row < 0 || row >= int(m_rows.size()) || m_rows[size_t(row)].status != Failed)
        return;
    request(row);
}

void HomeRowsModel::clear()
{
    m_libraries.clear();
    m_unplayedOnly = false;
    clearRows();
}

void HomeRowsModel::clearRows()
{
    if (m_rows.empty())
        return;
    beginResetModel();
    const std::vector<Row> old = std::move(m_rows);
    m_rows.clear();
    endResetModel();
    if (m_client)
        m_client->releaseListingChannel(FavoritesChannel);
    // The Repeater destroys its delegates after the reset: the inner lists may still be bound to these for a turn
    for (const Row &row : old)
        row.items->deleteLater();
}

void HomeRowsModel::request(int row)
{
    // Copied first: setStatus() emits, and a slot may rebuild the rows
    const Kind kind = m_rows[size_t(row)].kind;
    const int channel = m_rows[size_t(row)].channel;
    const QString libraryId = m_rows[size_t(row)].libraryId;
    const bool refreshing = m_rows[size_t(row)].status == Ready;
    if (refreshing)
        m_rows[size_t(row)].refreshing = true;
    else
        setStatus(row, Loading);
    bool started = false;
    if (m_client) {
        switch (kind) {
        case ContinueWatching:
            started = m_client->fetchResumeItems(ItemsPerRow, channel);
            break;
        case NextUp:
            started = m_client->fetchNextUp(
                QString(), QString(),
                QDate::currentDate().addDays(-NextUpDays).toString(QStringLiteral("yyyy-MM-dd")),
                false, ItemsPerRow, channel);
            break;
        case Favorites: {
            ItemQuery query;
            query.includeItemTypes = QStringLiteral("Movie,Series");
            query.recursive = true;
            query.filters = QStringLiteral("IsFavorite");
            query.sortBy = QStringLiteral("Random");
            // The season and episode counts of a series card
            query.fields = {QStringLiteral("PrimaryImageAspectRatio"), QStringLiteral("ChildCount"),
                            QStringLiteral("RecursiveItemCount")};
            query.cardImages = true;
            query.limit = ItemsPerRow;
            started = m_client->fetchItems(query, channel);
            break;
        }
        case RecentlyAdded:
            started = m_client->fetchLatestItems(libraryId, ItemsPerRow, channel,
                                                 m_unplayedOnly ? std::optional<bool>(false)
                                                                : std::nullopt);
            break;
        }
    }
    if (started)
        return;
    if (refreshing)
        m_rows[size_t(row)].refreshing = false;
    else
        setStatus(row, Failed);
}

int HomeRowsModel::rowFor(Kind kind, int channel) const
{
    for (int row = 0; row < int(m_rows.size()); ++row) {
        if (m_rows[size_t(row)].kind == kind && m_rows[size_t(row)].channel == channel)
            return row;
    }
    return -1;
}

void HomeRowsModel::deliver(Kind kind, int channel, const QJsonArray &items)
{
    const int row = rowFor(kind, channel);
    if (row < 0)
        return;
    Row &entry = m_rows[size_t(row)];
    if (entry.status != Loading && !entry.refreshing)
        return;
    const bool refreshed = std::exchange(entry.refreshing, false);
    entry.items->setItems(items);
    if (!refreshed) {
        setStatus(row, Ready);
        return;
    }
    const QModelIndex changed = index(row);
    Q_EMIT dataChanged(changed, changed, {CountRole, ShownRole});
}

void HomeRowsModel::fail(const QString &family, int channel)
{
    Kind kind;
    if (family == QLatin1String("Resume"))
        kind = ContinueWatching;
    else if (family == QLatin1String("NextUp"))
        kind = NextUp;
    else if (family == QLatin1String("Latest"))
        kind = RecentlyAdded;
    else if (family == QLatin1String("Items"))
        kind = Favorites;
    else
        return;
    const int row = rowFor(kind, channel);
    if (row < 0)
        return;
    // A refresh that fails leaves the row with what it showed
    if (std::exchange(m_rows[size_t(row)].refreshing, false))
        return;
    if (m_rows[size_t(row)].status == Loading)
        setStatus(row, Failed);
}

void HomeRowsModel::setStatus(int row, Status status)
{
    Row &entry = m_rows[size_t(row)];
    if (entry.status == status)
        return;
    entry.status = status;
    const QModelIndex changed = index(row);
    Q_EMIT dataChanged(changed, changed, {StatusRole, CountRole, ShownRole});
}

int HomeRowsModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QHash<int, QByteArray> HomeRowsModel::roleNames() const
{
    return {{KindRole, "kind"},     {LibraryIdRole, "libraryId"}, {LibraryNameRole, "libraryName"},
            {StatusRole, "status"}, {ItemsRole, "items"},         {LandscapeRole, "landscape"},
            {CountRole, "count"},   {ShownRole, "shown"}};
}

QVariant HomeRowsModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.parent().isValid() || index.row() < 0
        || index.row() >= int(m_rows.size()))
        return {};
    const Row &row = m_rows[size_t(index.row())];
    switch (role) {
    case KindRole:
        return row.kind;
    case LibraryIdRole:
        return row.libraryId;
    case LibraryNameRole:
        return row.libraryName;
    case StatusRole:
        return row.status;
    case ItemsRole:
        return QVariant::fromValue<QObject *>(row.items);
    case LandscapeRole:
        return row.items->landscape();
    case CountRole:
        return row.items->count();
    case ShownRole:
        // An empty row is hidden once it is loaded; a loading or failed one stays visible
        return row.status == Loading || row.status == Failed
               || (row.status == Ready && row.items->count() > 0);
    default:
        return {};
    }
}
} // namespace Kinefin
