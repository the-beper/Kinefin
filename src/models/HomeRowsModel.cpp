// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "HomeRowsModel.h"

#include "models/LibraryItemsModel.h"

#include <QDate>
#include <QMap>

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
        m_client, &JellyfinClient::latestItemsReady, this,
        [this](const QJsonArray &items, int channel) { deliver(RecentlyAdded, channel, items); });
    connect(m_client, &JellyfinClient::listingFailed, this,
            [this](const QString &family, int, const QString &, int channel) {
                fail(family, channel);
            });
    // Continue Watching, Next Up and Recently Added depend on the played flag.
    connect(m_client, &JellyfinClient::itemPlayedUpdated, this, [this] {
        if (!m_rows.empty())
            reload();
    });
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
    rows.push_back(
        {ContinueWatching, {}, {}, Idle, FixedChannel, new ItemRowModel(m_client, true, this)});
    rows.push_back({NextUp, {}, {}, Idle, FixedChannel, new ItemRowModel(m_client, true, this)});
    int index = 0;
    for (const QVariant &entry : std::as_const(m_libraries)) {
        const QVariantMap library = entry.toMap();
        const QString id = library.value(QStringLiteral("id")).toString();
        if (!hasRecentlyAdded(library.value(QStringLiteral("collectionType")).toString()))
            continue;
        rows.push_back({RecentlyAdded, id, library.value(QStringLiteral("name")).toString(), Idle,
                        FirstLatestChannel + index++,
                        new ItemRowModel(m_client, false, this, false)});
    }
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
        case RecentlyAdded:
            started = m_client->fetchLatestItems(libraryId, ItemsPerRow, channel,
                                                 m_unplayedOnly ? std::optional<bool>(false)
                                                                : std::nullopt);
            break;
        }
    }
    if (!started)
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
    if (row < 0 || m_rows[size_t(row)].status != Loading)
        return;
    m_rows[size_t(row)].items->setItems(items);
    setStatus(row, Ready);
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
    else
        return;
    const int row = rowFor(kind, channel);
    if (row >= 0 && m_rows[size_t(row)].status == Loading)
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
