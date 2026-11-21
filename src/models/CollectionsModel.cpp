// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "CollectionsModel.h"

#include <QJsonArray>

namespace Kinefin {

CollectionsModel::CollectionsModel(JellyfinClient *client, QObject *parent)
    : PagedItemsModel(PageSize, parent), m_client(client)
{
    if (m_client) {
        connect(m_client, &JellyfinClient::itemsReady, this, &CollectionsModel::onItemsReady);
        connect(m_client, &JellyfinClient::listingFailed, this, &CollectionsModel::onListingFailed);
        connect(m_client, &JellyfinClient::credentialsCleared, this, [this] { close(); });
    }
}

bool CollectionsModel::open()
{
    if (!m_client)
        return false;
    m_open = true;
    restartPaging();
    return status() != Failed;
}

void CollectionsModel::close()
{
    m_open = false;
    clearPaging();
    if (m_client)
        m_client->releaseListingChannel(Channel);
}

QHash<int, QByteArray> CollectionsModel::roleNames() const
{
    return {{ItemIdRole, "itemId"},
            {NameRole, "name"},
            {ImageUrlRole, "imageUrl"},
            {LoadedRole, "loaded"}};
}

QVariant CollectionsModel::rowData(const ItemSummary &item, bool loaded, int role) const
{
    switch (role) {
    case LoadedRole:
        return loaded;
    case ItemIdRole:
        return item.id;
    case NameRole:
    case Qt::DisplayRole:
        return item.name;
    case ImageUrlRole:
        return item.valid && m_client ? cardImageUrl(m_client, item, false, ImageWidth) : QUrl();
    default:
        return {};
    }
}

bool CollectionsModel::sendPage(int start, int limit)
{
    if (!m_client)
        return false;
    ItemQuery query;
    query.recursive = true;
    query.includeItemTypes = QStringLiteral("BoxSet");
    query.sortBy = QStringLiteral("SortName");
    query.cardImages = true;
    query.startIndex = start;
    query.limit = limit;
    return m_client->fetchItems(query, Channel);
}

void CollectionsModel::onItemsReady(const QJsonArray &items, int total, int channel)
{
    if (channel != Channel || !pageInFlight() || !m_open)
        return;
    deliverPage(items, total);
}

void CollectionsModel::onListingFailed(const QString &family, int, const QString &, int channel)
{
    if (family != QLatin1String("Items") || channel != Channel || !pageInFlight() || !m_open)
        return;
    failPageInFlight();
}
} // namespace Kinefin
