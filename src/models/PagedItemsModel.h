// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "models/ItemSummary.h"

#include <QAbstractListModel>
#include <QHash>
#include <QJsonArray>
#include <QList>
#include <QSet>
#include <QTimer>

#include <vector>

namespace Kinefin {
// A list of items that pages in on demand: the windowing shared by the library grid and the episodes of a season. `rowCount` is the server's `TotalRecordCount` once the first page arrived; a row whose page is not resident is a placeholder (`loaded` false) and reading it queues its page. One page is in flight at a time, the most recently read pages go first, and only the most recently used pages stay resident. Nothing retries by itself: a failure waits for `retry()`. A derived class sends the page request and feeds the reply back through `deliverPage()` or `failPageInFlight()`
class PagedItemsModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(Status status READ status NOTIFY statusChanged)
    Q_PROPERTY(int totalCount READ totalCount NOTIFY totalCountChanged)
    Q_PROPERTY(bool pageError READ pageError NOTIFY pageErrorChanged)

  public:
    enum Status { Idle, Loading, Ready, Failed };
    Q_ENUM(Status)

    static constexpr int MaxResidentPages = 8;
    static constexpr int MaxWantedPages = 4;
    // A larger TotalRecordCount from the server is clamped to this many rows
    static constexpr int MaxRows = 1'000'000;

    // Asks for the failed pages again, or for the first page when it never arrived
    Q_INVOKABLE virtual void retry();

    Status status() const { return m_status; }
    int totalCount() const { return m_total < 0 ? 0 : m_total; }
    bool pageError() const { return m_pageError; }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;

    // How many pages are loaded and in what order they were used, for the tests
    QList<int> residentPages() const { return m_residentOrder; }

  Q_SIGNALS:
    void statusChanged();
    void totalCountChanged();
    void pageErrorChanged();

  protected:
    PagedItemsModel(int pageSize, QObject *parent);

    // Sends the request of rows [start, start + limit); false when it is refused without a request
    virtual bool sendPage(int start, int limit) = 0;
    // The value of one role for a row; a placeholder row is an empty summary with `loaded` false
    virtual QVariant rowData(const ItemSummary &item, bool loaded, int role) const = 0;

    // Forgets every row and loads the first page
    void restartPaging();
    // Forgets every row and goes back to Idle
    void clearPaging();
    // Forgets every row and takes `status` (Loading or Failed) without asking for a page: for a derived class that cannot send yet, which then also overrides retry()
    void holdPaging(Status status);
    // Sets the favorite flag of the resident rows of an item and tells the views about `role`, the favorite role of the derived class
    void applyFavorite(const QString &itemId, bool favorite, int role);
    // Sets the watched state of the resident rows of an item, which also clears their progress; every role of the row is refreshed
    void applyPlayedState(const QString &itemId, bool played);
    // Sets the saved position of the resident rows of an item, as applyPlayedState does
    void applyProgressState(const QString &itemId, qint64 positionTicks, double playedPercentage);
    bool pageInFlight() const { return m_inFlight >= 0; }
    // The reply of the page in flight, or a failure of it
    void deliverPage(const QJsonArray &items, int total);
    void failPageInFlight();

  private:
    void resetState();
    void requestPage(int page);
    void pump();
    void noteWanted(int page) const;
    void touch(int page) const;
    void failPage(int page);
    void storePage(int page, const QJsonArray &items);
    void evictBeyondLimit(int keep);
    void dropPage(int page);
    void dropOtherPages(int keep);
    void resizeTo(int total);
    void setStatus(Status status);
    void setPageError(bool error);
    int rowsInPage(int page) const;

    const int m_pageSize;
    Status m_status = Idle;
    int m_total = -1;
    bool m_pageError = false;
    int m_inFlight = -1;
    QHash<int, std::vector<ItemSummary>> m_pages;
    QSet<int> m_failedPages;
    // Read-driven bookkeeping touched from the const data(): the pages asked for (newest last) and the resident pages by last use (newest last)
    mutable QList<int> m_wanted;
    mutable QList<int> m_residentOrder;
    mutable QTimer m_pumpTimer;
};
} // namespace Kinefin
