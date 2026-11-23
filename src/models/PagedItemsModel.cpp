// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "PagedItemsModel.h"

#include <QJsonValue>

#include <algorithm>

namespace Kinefin {

PagedItemsModel::PagedItemsModel(int pageSize, QObject *parent)
    : QAbstractListModel(parent), m_pageSize(pageSize)
{
    m_pumpTimer.setSingleShot(true);
    m_pumpTimer.setInterval(0);
    connect(&m_pumpTimer, &QTimer::timeout, this, [this] { pump(); });
}

void PagedItemsModel::restartPaging()
{
    resetState();
    setStatus(Loading);
    requestPage(0);
}

void PagedItemsModel::clearPaging()
{
    resetState();
    setStatus(Idle);
}

void PagedItemsModel::holdPaging(Status status)
{
    resetState();
    setStatus(status);
}

void PagedItemsModel::retry()
{
    if (m_status == Idle || m_inFlight >= 0)
        return;
    if (m_total < 0) {
        // The first page never arrived
        setStatus(Loading);
        requestPage(0);
        return;
    }
    const QSet<int> failed = m_failedPages;
    m_failedPages.clear();
    setPageError(false);
    for (const int page : failed)
        noteWanted(page);
}

void PagedItemsModel::requestPage(int page)
{
    m_inFlight = page;
    if (sendPage(page * m_pageSize, m_pageSize))
        return;
    // Refused without a request: not authenticated, or a query the client does not accept
    failPage(page);
}

void PagedItemsModel::failPageInFlight()
{
    if (m_inFlight >= 0)
        failPage(m_inFlight);
}

void PagedItemsModel::deliverPage(const QJsonArray &items, int total)
{
    if (m_inFlight < 0)
        return;
    total = std::min(total, MaxRows);
    const int page = m_inFlight;
    m_inFlight = -1;
    if (m_total < 0) {
        // The first page: the total and the placeholders appear together
        if (total > 0) {
            beginInsertRows(QModelIndex(), 0, total - 1);
            m_total = total;
            endInsertRows();
        } else {
            m_total = total;
        }
        Q_EMIT totalCountChanged();
    } else if (total != m_total) {
        // The list changed under the pages: only this reply is trusted
        dropOtherPages(page);
        resizeTo(total);
    }
    m_failedPages.remove(page);
    storePage(page, items);
    setPageError(!m_failedPages.isEmpty());
    setStatus(Ready);
    if (!m_wanted.isEmpty())
        m_pumpTimer.start();
}

QVariant PagedItemsModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.parent().isValid() || index.row() < 0
        || index.row() >= totalCount())
        return {};
    const int page = index.row() / m_pageSize;
    const auto resident = m_pages.constFind(page);
    const int offset = index.row() % m_pageSize;
    const bool loaded = resident != m_pages.constEnd() && offset < int(resident->size());
    if (resident != m_pages.constEnd())
        touch(page);
    else
        noteWanted(page);
    const ItemSummary empty;
    const ItemSummary &item = loaded ? resident->at(offset) : empty;
    return rowData(item, loaded && item.valid, role);
}

void PagedItemsModel::resetState()
{
    beginResetModel();
    m_pumpTimer.stop();
    m_pages.clear();
    m_failedPages.clear();
    m_wanted.clear();
    m_residentOrder.clear();
    m_inFlight = -1;
    const bool hadTotal = m_total >= 0;
    m_total = -1;
    endResetModel();
    if (hadTotal)
        Q_EMIT totalCountChanged();
    setPageError(false);
}

int PagedItemsModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : totalCount();
}

int PagedItemsModel::rowsInPage(int page) const
{
    return std::clamp(m_total - page * m_pageSize, 0, int(m_pageSize));
}

void PagedItemsModel::noteWanted(int page) const
{
    if (m_status == Idle || m_failedPages.contains(page) || page == m_inFlight
        || m_pages.contains(page))
        return;
    m_wanted.removeAll(page);
    m_wanted.append(page);
    while (m_wanted.size() > MaxWantedPages)
        m_wanted.removeFirst();
    // Not from inside data(): the request goes out on the next event loop turn
    if (!m_pumpTimer.isActive())
        m_pumpTimer.start();
}

void PagedItemsModel::touch(int page) const
{
    if (!m_residentOrder.isEmpty() && m_residentOrder.last() == page)
        return;
    m_residentOrder.removeAll(page);
    m_residentOrder.append(page);
}

void PagedItemsModel::pump()
{
    if (m_inFlight >= 0 || m_status == Idle || m_total < 0)
        return;
    while (!m_wanted.isEmpty()) {
        const int page = m_wanted.takeLast();
        if (m_pages.contains(page) || m_failedPages.contains(page) || rowsInPage(page) == 0)
            continue;
        requestPage(page);
        return;
    }
}

void PagedItemsModel::storePage(int page, const QJsonArray &items)
{
    const int expected = rowsInPage(page);
    if (expected == 0)
        return;
    std::vector<ItemSummary> rows;
    rows.reserve(std::min<qsizetype>(items.size(), expected));
    for (const QJsonValue &value : items) {
        if (int(rows.size()) == expected)
            break;
        rows.push_back(parseItemSummary(value.toObject()));
    }
    const int received = int(rows.size());
    m_pages.insert(page, std::move(rows));
    touch(page);
    if (received < expected) {
        // A short page with an unchanged total: the library ends here, and asking again would loop
        resizeTo(page * m_pageSize + received);
    }
    const int first = page * m_pageSize;
    if (received > 0)
        Q_EMIT dataChanged(index(first), index(first + received - 1));
    evictBeyondLimit(page);
}

void PagedItemsModel::applyFavorite(const QString &itemId, bool favorite, int role)
{
    for (auto page = m_pages.begin(); page != m_pages.end(); ++page) {
        for (size_t i = 0; i < page->size(); ++i) {
            ItemSummary &item = (*page)[i];
            if (item.favorite == favorite || !item.valid || !sameItemId(item.id, itemId))
                continue;
            item.favorite = favorite;
            const QModelIndex row = index(page.key() * m_pageSize + int(i));
            Q_EMIT dataChanged(row, row, {role});
        }
    }
}

void PagedItemsModel::updateResident(const QString &itemId,
                                     const std::function<void(ItemSummary &)> &change)
{
    for (auto page = m_pages.begin(); page != m_pages.end(); ++page) {
        for (size_t i = 0; i < page->size(); ++i) {
            ItemSummary &item = (*page)[i];
            if (!item.valid || !sameItemId(item.id, itemId))
                continue;
            change(item);
            const QModelIndex row = index(page.key() * m_pageSize + int(i));
            Q_EMIT dataChanged(row, row);
        }
    }
}

void PagedItemsModel::applyPlayedState(const QString &itemId, bool played)
{
    updateResident(itemId, [played](ItemSummary &item) { applyPlayed(item, played); });
}

void PagedItemsModel::applyProgressState(const QString &itemId, qint64 positionTicks,
                                         double playedPercentage)
{
    updateResident(itemId, [positionTicks, playedPercentage](ItemSummary &item) {
        applyProgress(item, positionTicks, playedPercentage);
    });
}

void PagedItemsModel::applyUnplayedCountState(const QString &itemId, int unplayedCount)
{
    updateResident(itemId,
                   [unplayedCount](ItemSummary &item) { item.unplayedCount = unplayedCount; });
}

void PagedItemsModel::evictBeyondLimit(int keep)
{
    while (m_residentOrder.size() > MaxResidentPages) {
        const int oldest = m_residentOrder.first();
        if (oldest == keep)
            break;
        dropPage(oldest);
    }
}

void PagedItemsModel::dropPage(int page)
{
    m_residentOrder.removeAll(page);
    const auto resident = m_pages.take(page);
    if (!resident.empty()) {
        const int first = page * m_pageSize;
        Q_EMIT dataChanged(index(first), index(first + int(resident.size()) - 1));
    }
}

void PagedItemsModel::dropOtherPages(int keep)
{
    const QList<int> pages = m_pages.keys();
    for (const int page : pages) {
        if (page != keep)
            dropPage(page);
    }
    m_wanted.clear();
    m_failedPages.clear();
}

void PagedItemsModel::resizeTo(int total)
{
    total = std::max(total, 0);
    const int current = totalCount();
    if (total > current) {
        beginInsertRows(QModelIndex(), current, total - 1);
        m_total = total;
        endInsertRows();
    } else if (total < current) {
        beginRemoveRows(QModelIndex(), total, current - 1);
        m_total = total;
        // Pages entirely past the new end are gone with their rows
        for (const int page : m_pages.keys()) {
            if (page * m_pageSize >= total) {
                m_pages.remove(page);
                m_residentOrder.removeAll(page);
            } else if (int(m_pages.value(page).size()) > total - page * m_pageSize) {
                m_pages[page].resize(size_t(total - page * m_pageSize));
            }
        }
        endRemoveRows();
        // Pages past the new end are gone: they must not keep the error flag or a queued request alive
        m_failedPages.removeIf([this](int page) { return page * m_pageSize >= m_total; });
        m_wanted.removeIf([this](int page) { return page * m_pageSize >= m_total; });
        setPageError(!m_failedPages.isEmpty());
    } else {
        return;
    }
    Q_EMIT totalCountChanged();
}

void PagedItemsModel::failPage(int page)
{
    m_inFlight = -1;
    if (m_total < 0) {
        setStatus(Failed);
        return;
    }
    m_failedPages.insert(page);
    setPageError(true);
    if (!m_wanted.isEmpty())
        m_pumpTimer.start();
}

void PagedItemsModel::setStatus(Status status)
{
    if (status == m_status)
        return;
    m_status = status;
    Q_EMIT statusChanged();
}

void PagedItemsModel::setPageError(bool error)
{
    if (error == m_pageError)
        return;
    m_pageError = error;
    Q_EMIT pageErrorChanged();
}

} // namespace Kinefin
